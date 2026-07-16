# Memory & VRAM Viewer

A low-profile Windows desktop tool that shows what's *actually* occupying your
memory — including the private-commit (page-file-backed) numbers Task Manager's
per-process list hides — plus real system-wide VRAM usage.

Built with **C++ + Dear ImGui** (Win32 + DirectX 11). GPU-drawn, dark UI, no
.NET runtime. Idle footprint ~50 MB working set (mostly shared GPU driver DLLs).

The UI has two tabs:

**Overview** — system RAM, commit charge, kernel pools, VRAM totals, and a
per-process table (private commit vs working set).

**GPU Processes** — per-process GPU memory. Leads with the *physically resident*
VRAM (the real, card-capped number), then lists per-process **committed**
allocations. Because GPU memory is virtualized (WDDM), these overlap and sum to
far more than the card holds: the compositor (`dwm`) double-counts every visible
window's surface, and capture/overlay processes over-report — both are flagged
inline so the numbers aren't mistaken for real occupancy.

## What it shows

- **Physical RAM** — used / total with a load bar.
- **Commit charge** — used / limit (RAM + page files). This is the number your
  page file backs; it's what prevents out-of-memory crashes.
- **Kernel pools, cache, process/thread/handle counts.**
- **VRAM per GPU** — dedicated and shared, *system-wide* usage vs total
  (via PDH performance counters, the same source Task Manager uses). Phantom
  duplicate adapters are filtered out.
- **Process table** — sortable, showing both:
  - **Private commit** (`PrivateUsage`) — RAM *and/or* page file. The page-file
    eaters. Sorted here by default.
  - **Working set** (`WorkingSetSize`) — physical RAM only (what Task Manager's
    "Memory" column shows). The gap between the two = memory paged to disk.

## Build

Requires Visual Studio 2022/2026 (MSVC + Windows SDK) and git. No CMake needed.

After a fresh clone, install the vendored dependency (Dear ImGui, pinned to a
release tag) and build:

```bat
setup.bat     :: like `npm install` - fetches third_party\imgui @ pinned tag
build.bat     :: compiles build\MemoryViewer.exe
```

`build.bat` auto-runs `setup.bat` if the dependency is missing, so `build.bat`
alone works too. PowerShell users can run `.\setup.ps1` instead.

To change the pinned Dear ImGui version, edit `IMGUI_TAG` in `setup.bat`
(and `$imguiTag` in `setup.ps1`).

If your Visual Studio lives elsewhere, edit the `VCVARS` path near the top of
`build.bat`.

## Layout

```
src/metrics.h      data structs + query API
src/metrics.cpp    Win32 (RAM/commit/processes) + DXGI/PDH (VRAM) queries
src/main.cpp       ImGui + DX11 host window and UI
third_party/imgui  Dear ImGui (vendored via git, not committed)
build.bat          one-shot MSVC build
```

## How the numbers are sourced

| Metric | API |
|---|---|
| Physical RAM, page file | `GlobalMemoryStatusEx` |
| Commit total/limit/peak, pools, cache, counts | `GetPerformanceInfo` |
| Per-process working set + private commit | `EnumProcesses` + `GetProcessMemoryInfo` (`PROCESS_MEMORY_COUNTERS_EX`) |
| VRAM totals + adapter enumeration | DXGI (`IDXGIAdapter3`, `DXGI_ADAPTER_DESC1`) |
| VRAM system-wide usage (dedicated/shared) | PDH `\GPU Adapter Memory(*)\Dedicated Usage` / `Shared Usage` |

## Ideas / next steps

- Per-process GPU memory (PDH `\GPU Process Memory(*)\...`).
- Scrolling history graphs (ImGui `PlotLines`) for RAM/commit/VRAM.
- Monitor/output list per adapter (`IDXGIAdapter::EnumOutputs`).
- Right-click a process to end it or open its file location.
- Minimize-to-tray for always-on monitoring.
```
