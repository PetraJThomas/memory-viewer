// metrics.cpp - implementation of the memory/VRAM queries.
#include "metrics.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <algorithm>
#include <cstdio>

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

// ---------------------------------------------------------------------------
// System memory
// ---------------------------------------------------------------------------
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
    }
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

    PDH_HQUERY   g_pdhQuery      = nullptr;
    PDH_HCOUNTER g_cDedicated    = nullptr;
    PDH_HCOUNTER g_cShared       = nullptr;

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

    // PDH counters for system-wide GPU memory (English names => locale-proof).
    if (PdhOpenQueryW(nullptr, 0, &g_pdhQuery) == ERROR_SUCCESS) {
        PdhAddEnglishCounterA(g_pdhQuery, "\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &g_cDedicated);
        PdhAddEnglishCounterA(g_pdhQuery, "\\GPU Adapter Memory(*)\\Shared Usage",    0, &g_cShared);
        PdhCollectQueryData(g_pdhQuery); // prime
    }
    return !g_adapters.empty();
}

void ShutdownVram() {
    if (g_pdhQuery) { PdhCloseQuery(g_pdhQuery); g_pdhQuery = nullptr; }
    g_cDedicated = g_cShared = nullptr;
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
