#pragma once
// metrics.h - low-level system/GPU memory queries.
// Everything here is plain Win32 + DXGI; no allocations on the hot path
// beyond growing the process vector, so the viewer stays low-profile.

#include <cstdint>
#include <string>
#include <vector>

// System-wide physical RAM + commit charge (the page-file-backed number
// Task Manager hides on its per-process list).
struct SystemMemory {
    // Physical RAM
    uint64_t physTotal   = 0;
    uint64_t physAvail   = 0;
    uint64_t physUsed    = 0;   // physTotal - physAvail
    double   physPercent = 0.0;

    // Commit charge: memory the OS has promised (RAM + page files).
    // This is what your huge page file protects; commitLimit is RAM + pagefiles.
    uint64_t commitTotal   = 0;
    uint64_t commitLimit   = 0;
    uint64_t commitPeak    = 0;
    double   commitPercent = 0.0;

    // Cache & kernel pools
    uint64_t systemCache    = 0;
    uint64_t kernelPaged    = 0;
    uint64_t kernelNonpaged = 0;

    // Memory compression: cold pages compressed and kept *in RAM* (warm) by the
    // "Memory Compression" process instead of being paged to disk (cold).
    uint64_t compressed = 0; // size of the compression store in RAM

    // Counts (from GetPerformanceInfo)
    uint32_t processCount = 0;
    uint32_t threadCount  = 0;
    uint32_t handleCount  = 0;

    uint32_t memoryLoad = 0; // dwMemoryLoad, 0..100

    // Page file(s) on disk (authoritative, from NtQuerySystemInformation).
    // pageFileInUse is the portion of commit that's actually paged out to disk.
    uint64_t pageFileTotal = 0; // sum of page file sizes
    uint64_t pageFileInUse = 0; // currently stored in the page file
};

// Per-process memory. The key field is privateBytes (private commit):
// the memory that lives in RAM *and/or* your page file and that the
// Task Manager "Memory" column (working set only) never shows you.
struct ProcessMemory {
    uint32_t    pid = 0;
    std::string name;          // UTF-8
    uint64_t    workingSet   = 0; // physical RAM currently used
    uint64_t    privateBytes = 0; // private commit (RAM + page file)
    bool        accessible   = true;
};

// One GPU adapter's video memory, split the way the Task Manager GPU tab shows it.
// Totals come from DXGI; the *system-wide* usage comes from PDH performance
// counters (DXGI's own usage counter is only this process's slice).
struct AdapterVram {
    std::string name;              // UTF-8
    uint64_t dedicatedTotal = 0;   // on-board VRAM (physical)
    uint64_t sharedTotal    = 0;   // system RAM the GPU may borrow

    uint64_t dedicatedUsage = 0;   // system-wide dedicated in use (all processes)
    uint64_t sharedUsage    = 0;   // system-wide shared in use
    bool     hasUsage       = false;

    // LUID used to match the DXGI adapter to its PDH counter instance.
    unsigned long luidLow  = 0;
    long          luidHigh = 0;
};

// Per-process GPU memory (what each process is holding in VRAM), from the
// PDH "GPU Process Memory" counter set -- the same source as Task Manager's
// per-process "Dedicated GPU memory" column.
struct ProcessVram {
    uint32_t    pid = 0;
    std::string name;           // UTF-8
    uint64_t    dedicated = 0;  // on-board VRAM held by this process
    uint64_t    shared    = 0;  // shared (system RAM) GPU memory held
};

// --- API -------------------------------------------------------------------
bool QuerySystemMemory(SystemMemory& out);

// Fills `out` with one entry per process, sorted by privateBytes desc.
// `outAccessiblePrivateSum` receives the total private commit we could read.
void QueryProcesses(std::vector<ProcessMemory>& out, uint64_t& outAccessiblePrivateSum);

// DXGI must be initialised once before QueryVram is called.
bool InitVram();
void ShutdownVram();
void QueryVram(std::vector<AdapterVram>& out);

// Per-process GPU memory. Must be called after QueryVram in the same refresh
// tick (they share one PDH sample). Sorted by dedicated usage desc.
void QueryProcessVram(std::vector<ProcessVram>& out);
