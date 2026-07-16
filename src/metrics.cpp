// metrics.cpp - implementation of the memory/VRAM queries.
#include "metrics.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <winternl.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "pdh.lib")

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
static std::string WideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

// Resolve a PID to its executable basename (UTF-8), or "<pid N>" if we can't.
static std::string ProcessBaseName(DWORD pid) {
    if (pid == 0) return "System Idle";
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    std::string name;
    if (h) {
        wchar_t path[MAX_PATH];
        DWORD len = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &len) && len > 0) {
            const wchar_t* base = path;
            for (const wchar_t* p = path; *p; ++p)
                if (*p == L'\\' || *p == L'/') base = p + 1;
            name = WideToUtf8(base);
        }
        CloseHandle(h);
    }
    if (name.empty()) name = "<pid " + std::to_string(pid) + ">";
    return name;
}

// Map every PID -> executable basename via a Toolhelp snapshot. Unlike
// OpenProcess this also names protected/system processes (dwm, csrss, ...).
static void BuildPidNameMap(std::vector<std::pair<DWORD, std::string>>& map) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do { map.emplace_back(pe.th32ProcessID, WideToUtf8(pe.szExeFile)); }
        while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

// ---------------------------------------------------------------------------
// System memory
// ---------------------------------------------------------------------------

// Compression-store size = working set of the "Memory Compression" process.
// Task Manager's source; there is no performance counter for it.
static uint64_t QueryCompressedBytes() {
    typedef LONG (NTAPI* PFN_NQSI)(ULONG, PVOID, ULONG, PULONG);
    static PFN_NQSI nq = (PFN_NQSI)GetProcAddress(
        GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    if (!nq) return 0;

    ULONG need = 512 * 1024;
    std::vector<BYTE> buf;
    LONG st = 0;
    for (int tries = 0; tries < 8; ++tries) {
        buf.resize(need);
        ULONG ret = 0;
        st = nq(5 /*SystemProcessInformation*/, buf.data(), need, &ret);
        if ((ULONG)st != 0xC0000004UL /*STATUS_INFO_LENGTH_MISMATCH*/) break;
        need = (ret > need) ? ret + 65536 : need * 2;
    }
    if (st != 0) return 0;

    for (BYTE* p = buf.data();;) {
        auto* e = reinterpret_cast<SYSTEM_PROCESS_INFORMATION*>(p);
        if (e->ImageName.Buffer && e->ImageName.Length == 18 * sizeof(wchar_t) &&
            wcsncmp(e->ImageName.Buffer, L"Memory Compression", 18) == 0)
            return (uint64_t)e->WorkingSetSize;
        if (e->NextEntryOffset == 0) break;
        p += e->NextEntryOffset;
    }
    return 0;
}

bool QuerySystemMemory(SystemMemory& out) {
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return false;

    out.physTotal   = ms.ullTotalPhys;
    out.physAvail   = ms.ullAvailPhys;
    out.physUsed    = ms.ullTotalPhys - ms.ullAvailPhys;
    out.physPercent = out.physTotal ? (double)out.physUsed / out.physTotal : 0.0;
    out.memoryLoad  = ms.dwMemoryLoad;

    PERFORMANCE_INFORMATION pi{};
    pi.cb = sizeof(pi);
    if (GetPerformanceInfo(&pi, sizeof(pi))) {
        const uint64_t pg = pi.PageSize;
        out.commitTotal    = (uint64_t)pi.CommitTotal * pg;
        out.commitLimit    = (uint64_t)pi.CommitLimit * pg;
        out.commitPeak     = (uint64_t)pi.CommitPeak  * pg;
        out.commitPercent  = pi.CommitLimit ? (double)pi.CommitTotal / pi.CommitLimit : 0.0;
        out.systemCache    = (uint64_t)pi.SystemCache    * pg;
        out.kernelPaged    = (uint64_t)pi.KernelPaged    * pg;
        out.kernelNonpaged = (uint64_t)pi.KernelNonpaged * pg;
        out.processCount   = pi.ProcessCount;
        out.threadCount    = pi.ThreadCount;
        out.handleCount    = pi.HandleCount;

        // Real page file size + usage. MEMORYSTATUSEX's "page file" fields are
        // actually the commit limit, so ask the kernel directly.
        // SystemPageFileInformation = 18; sizes are in pages.
        typedef LONG (NTAPI* PFN_NQSI)(ULONG, PVOID, ULONG, PULONG);
        struct PF_ENTRY { ULONG NextEntryOffset, TotalSize, TotalInUse, PeakUsage; };
        static PFN_NQSI nq = (PFN_NQSI)GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
        if (nq) {
            std::vector<BYTE> buf(65536);
            ULONG ret = 0;
            if (nq(18, buf.data(), (ULONG)buf.size(), &ret) == 0 && ret >= sizeof(PF_ENTRY)) {
                // First entry = primary page file (covers ~all systems). Reading
                // only it avoids the duplicate-entry over-count some builds return.
                auto* e = reinterpret_cast<PF_ENTRY*>(buf.data());
                out.pageFileTotal = (uint64_t)e->TotalSize  * pg;
                out.pageFileInUse = (uint64_t)e->TotalInUse * pg;
            }
        }
        // Fallback / sanity: page file size = commit limit - physical RAM.
        if (out.pageFileTotal == 0 && out.commitLimit > out.physTotal)
            out.pageFileTotal = out.commitLimit - out.physTotal;
    }
    out.compressed = QueryCompressedBytes();
    return true;
}

// ---------------------------------------------------------------------------
// Per-process memory
// ---------------------------------------------------------------------------
void QueryProcesses(std::vector<ProcessMemory>& out, uint64_t& outAccessiblePrivateSum) {
    out.clear();
    outAccessiblePrivateSum = 0;

    // Enumerate PIDs. Grow the buffer until it isn't fully filled.
    std::vector<DWORD> pids(1024);
    DWORD needed = 0;
    for (;;) {
        DWORD cb = (DWORD)(pids.size() * sizeof(DWORD));
        if (!EnumProcesses(pids.data(), cb, &needed)) return;
        if (needed < cb) break;
        pids.resize(pids.size() * 2);
    }
    const size_t count = needed / sizeof(DWORD);
    out.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        DWORD pid = pids[i];
        if (pid == 0) continue; // System Idle Process

        ProcessMemory pm;
        pm.pid = pid;

        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) {
            // Protected/system process we can't inspect.
            pm.name       = "<pid " + std::to_string(pid) + ">";
            pm.accessible = false;
            out.push_back(std::move(pm));
            continue;
        }

        // Image name (basename only).
        wchar_t path[MAX_PATH];
        DWORD pathLen = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, path, &pathLen) && pathLen > 0) {
            const wchar_t* base = path;
            for (const wchar_t* p = path; *p; ++p)
                if (*p == L'\\' || *p == L'/') base = p + 1;
            pm.name = WideToUtf8(base);
        }
        if (pm.name.empty()) pm.name = "<pid " + std::to_string(pid) + ">";

        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof(pmc);
        if (GetProcessMemoryInfo(h, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc))) {
            pm.workingSet   = pmc.WorkingSetSize;
            pm.privateBytes = pmc.PrivateUsage; // private commit
            outAccessiblePrivateSum += pm.privateBytes;
        } else {
            pm.accessible = false;
        }
        CloseHandle(h);
        out.push_back(std::move(pm));
    }

    std::sort(out.begin(), out.end(), [](const ProcessMemory& a, const ProcessMemory& b) {
        return a.privateBytes > b.privateBytes;
    });
}

// ---------------------------------------------------------------------------
// VRAM: adapter totals from DXGI, system-wide usage from PDH.
// ---------------------------------------------------------------------------
namespace {
    ComPtr<IDXGIFactory1>              g_factory;
    std::vector<ComPtr<IDXGIAdapter3>> g_adapters;

    PDH_HQUERY   g_pdhQuery       = nullptr;
    PDH_HCOUNTER g_cDedicated     = nullptr;  // adapter-wide dedicated
    PDH_HCOUNTER g_cShared        = nullptr;  // adapter-wide shared
    PDH_HCOUNTER g_cProcDedicated = nullptr;  // per-process dedicated
    PDH_HCOUNTER g_cProcShared    = nullptr;  // per-process shared

    // Sum PDH "GPU Adapter Memory" instances for a given LUID.
    // Instance names look like: "luid_0x00000000_0x0000A1B2_phys_0".
    // `matched` is set true if any instance for this LUID existed at all --
    // that lets us drop phantom duplicate adapters the OS doesn't track.
    uint64_t SumForLuid(PDH_HCOUNTER counter, unsigned long low, long high, bool& matched) {
        matched = false;
        DWORD bufSize = 0, itemCount = 0;
        PDH_STATUS st = PdhGetFormattedCounterArrayA(counter, PDH_FMT_LARGE,
                                                     &bufSize, &itemCount, nullptr);
        if (st != PDH_MORE_DATA || bufSize == 0) return 0;

        std::vector<BYTE> buf(bufSize);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_A*>(buf.data());
        if (PdhGetFormattedCounterArrayA(counter, PDH_FMT_LARGE,
                                         &bufSize, &itemCount, items) != ERROR_SUCCESS)
            return 0;

        uint64_t total = 0;
        for (DWORD i = 0; i < itemCount; ++i) {
            unsigned int h = 0, l = 0, phys = 0;
            if (sscanf_s(items[i].szName, "luid_0x%x_0x%x_phys_%u", &h, &l, &phys) >= 2) {
                if ((long)h == high && (unsigned long)l == low) {
                    matched = true;
                    if (items[i].FmtValue.CStatus == ERROR_SUCCESS)
                        total += (uint64_t)items[i].FmtValue.largeValue;
                }
            }
        }
        return total;
    }
}

bool InitVram() {
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&g_factory))))
        return false;

    std::vector<LUID> seen;
    ComPtr<IDXGIAdapter1> a1;
    for (UINT i = 0; g_factory->EnumAdapters1(i, &a1) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc{};
        a1->GetDesc1(&desc);
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { a1.Reset(); continue; }

        // De-duplicate: some systems enumerate the same physical GPU repeatedly.
        bool dup = false;
        for (const LUID& l : seen)
            if (l.LowPart == desc.AdapterLuid.LowPart && l.HighPart == desc.AdapterLuid.HighPart)
                { dup = true; break; }
        if (dup) { a1.Reset(); continue; }
        seen.push_back(desc.AdapterLuid);

        ComPtr<IDXGIAdapter3> a3;
        if (SUCCEEDED(a1.As(&a3)))
            g_adapters.push_back(a3);
        a1.Reset();
    }

    // PDH counters for GPU memory (English names => locale-proof).
    if (PdhOpenQueryW(nullptr, 0, &g_pdhQuery) == ERROR_SUCCESS) {
        PdhAddEnglishCounterA(g_pdhQuery, "\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &g_cDedicated);
        PdhAddEnglishCounterA(g_pdhQuery, "\\GPU Adapter Memory(*)\\Shared Usage",    0, &g_cShared);
        PdhAddEnglishCounterA(g_pdhQuery, "\\GPU Process Memory(*)\\Dedicated Usage", 0, &g_cProcDedicated);
        PdhAddEnglishCounterA(g_pdhQuery, "\\GPU Process Memory(*)\\Shared Usage",    0, &g_cProcShared);
        PdhCollectQueryData(g_pdhQuery); // prime
    }
    return !g_adapters.empty();
}

void ShutdownVram() {
    if (g_pdhQuery) { PdhCloseQuery(g_pdhQuery); g_pdhQuery = nullptr; }
    g_cDedicated = g_cShared = g_cProcDedicated = g_cProcShared = nullptr;
    g_adapters.clear();
    g_factory.Reset();
}

void QueryVram(std::vector<AdapterVram>& out) {
    out.clear();

    bool pdhOk = g_pdhQuery && PdhCollectQueryData(g_pdhQuery) == ERROR_SUCCESS;

    for (auto& a3 : g_adapters) {
        DXGI_ADAPTER_DESC1 desc{};
        a3->GetDesc1(&desc);

        AdapterVram v;
        v.name           = WideToUtf8(desc.Description);
        v.dedicatedTotal = desc.DedicatedVideoMemory;
        v.sharedTotal    = desc.SharedSystemMemory;
        v.luidLow        = desc.AdapterLuid.LowPart;
        v.luidHigh       = desc.AdapterLuid.HighPart;

        if (pdhOk && g_cDedicated && g_cShared) {
            bool mDed = false, mShr = false;
            v.dedicatedUsage = SumForLuid(g_cDedicated, v.luidLow, v.luidHigh, mDed);
            v.sharedUsage    = SumForLuid(g_cShared,    v.luidLow, v.luidHigh, mShr);
            v.hasUsage       = true;
            // Phantom duplicate adapter the OS tracks no memory for -> skip it.
            if (!mDed && !mShr) continue;
        }
        out.push_back(std::move(v));
    }
}

namespace {
    // Accumulate a PDH "GPU Process Memory" counter array into a pid->bytes map.
    // Instance names look like: "pid_1234_luid_0x00000000_0x0000A1B2_phys_0".
    void AccumulateProcVram(PDH_HCOUNTER counter, std::vector<std::pair<DWORD,uint64_t>>& acc) {
        if (!counter) return;
        DWORD bufSize = 0, itemCount = 0;
        if (PdhGetFormattedCounterArrayA(counter, PDH_FMT_LARGE, &bufSize, &itemCount, nullptr)
                != PDH_MORE_DATA || bufSize == 0)
            return;

        std::vector<BYTE> buf(bufSize);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_A*>(buf.data());
        if (PdhGetFormattedCounterArrayA(counter, PDH_FMT_LARGE, &bufSize, &itemCount, items)
                != ERROR_SUCCESS)
            return;

        for (DWORD i = 0; i < itemCount; ++i) {
            unsigned int pid = 0, h = 0, l = 0, phys = 0;
            if (sscanf_s(items[i].szName, "pid_%u_luid_0x%x_0x%x_phys_%u", &pid, &h, &l, &phys) >= 1 &&
                items[i].FmtValue.CStatus == ERROR_SUCCESS) {
                uint64_t val = (uint64_t)items[i].FmtValue.largeValue;
                // Sum all luid/phys segments belonging to this pid.
                bool found = false;
                for (auto& e : acc)
                    if (e.first == pid) { e.second += val; found = true; break; }
                if (!found) acc.emplace_back((DWORD)pid, val);
            }
        }
    }
}

void QueryProcessVram(std::vector<ProcessVram>& out) {
    out.clear();
    if (!g_pdhQuery) return;

    std::vector<std::pair<DWORD,uint64_t>> ded, shr;
    AccumulateProcVram(g_cProcDedicated, ded);
    AccumulateProcVram(g_cProcShared,    shr);

    // Merge dedicated + shared per pid.
    for (auto& d : ded) {
        ProcessVram pv;
        pv.pid = d.first;
        pv.dedicated = d.second;
        out.push_back(pv);
    }
    for (auto& s : shr) {
        bool found = false;
        for (auto& pv : out)
            if (pv.pid == s.first) { pv.shared = s.second; found = true; break; }
        if (!found) { ProcessVram pv; pv.pid = s.first; pv.shared = s.second; out.push_back(pv); }
    }

    // Drop processes holding nothing, then name + sort the rest.
    out.erase(std::remove_if(out.begin(), out.end(),
              [](const ProcessVram& p){ return p.dedicated == 0 && p.shared == 0; }), out.end());

    // Resolve names via Toolhelp so protected processes (dwm, csrss) are named.
    std::vector<std::pair<DWORD, std::string>> nameMap;
    BuildPidNameMap(nameMap);
    for (auto& pv : out) {
        for (auto& e : nameMap)
            if (e.first == pv.pid) { pv.name = e.second; break; }
        if (pv.name.empty()) pv.name = ProcessBaseName(pv.pid); // fallback
    }
    std::sort(out.begin(), out.end(), [](const ProcessVram& a, const ProcessVram& b) {
        if (a.dedicated != b.dedicated) return a.dedicated > b.dedicated;
        return a.shared > b.shared;
    });
}
