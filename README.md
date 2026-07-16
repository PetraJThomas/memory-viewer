# RV MEM Viewer

A low-profile Windows desktop tool that shows what's **actually** occupying your
memory — including the private-commit and page-file numbers Task Manager hides —
plus true system-wide and per-process VRAM usage.

Built with **C++ + Dear ImGui** (Win32 + DirectX 11). GPU-drawn, custom
gold/red theme, no .NET runtime. Idle footprint ~50 MB working set (mostly
shared GPU driver DLLs).

## Why it exists

Task Manager's per-process **Memory** column only shows the *working set* —
physical RAM. It never shows **private commit**, the memory a process has
committed that may be sitting in your **page file**. On a machine with a large
page file (to survive heavy load without out-of-memory crashes), the commit
charge can be tens of GB while the working sets stay small — and Task Manager
gives you no way to see *who* is filling it. This tool does.

It also untangles GPU memory, where Windows' virtualized (WDDM) accounting makes
per-process numbers wildly exceed the physical card — showing the **real
resident** number alongside the misleading committed ones.

## The three tabs

**Dashboard** — live ring gauges for **RAM**, **commit charge**, and **VRAM**,
a 2-minute history graph, a **page file** bar (paged-out commit vs total page
file), and detail chips (available, cached, commit peak, kernel pools,
process/thread/handle counts).

**Processes** — a headline card (physical RAM, commit charge, page file) then a
sortable table of every process showing **private commit** vs **working set**.
The gap between the two columns is memory paged to disk — the private-commit
column has an inline heat bar so the page-file eaters jump out.

**GPU Memory** — leads with the **physically resident** VRAM (the real,
card-capped number from PDH), then per-process **committed** allocations.
Because GPU memory is virtualized, these overlap and sum to far more than the
card holds: the compositor (`dwm`) double-counts every visible window's surface,
and capture/overlay processes over-report — both are flagged inline so the
numbers aren't mistaken for real occupancy.

## Key concepts it makes visible

| Pair | What the difference tells you |
|---|---|
| Private commit vs working set | How much of a process's memory is paged to disk vs in RAM |
| Commit charge vs commit limit | How close you are to an out-of-memory allocation failure |
| Page file in use vs total | How much of your page file is actually holding paged-out commit |
| Resident VRAM vs per-process committed | What's really on the card vs virtualized/double-counted accounting |

## Download

Grab **RVMemViewer.exe** from the [Releases](../../releases) page and run it -
it's self-contained (static CRT, only system DLLs + fonts), no installer.
Requires Windows 10/11 x64.

> **Windows SmartScreen note:** the release binary is unsigned (no paid
> code-signing certificate), so on first launch SmartScreen may show
> *"Windows protected your PC."* This is expected for indie/unsigned apps, not a
> sign of anything wrong. To run it: click **More info -> Run anyway**. If you'd
> rather not trust the prebuilt binary, build it yourself from source (below) -
> the result is identical.

## Build

Requires Visual Studio 2022/2026 (MSVC + Windows SDK) and git. No CMake needed.

```bat
setup.bat     :: like `npm install` - fetches third_party\imgui @ a pinned tag
build.bat     :: compiles resources + build\RVMemViewer.exe
```

`build.bat` auto-runs `setup.bat` if Dear ImGui is missing, so `build.bat` alone
works on a fresh clone. PowerShell users can run `.\setup.ps1`. To bump the
pinned ImGui version, edit `IMGUI_TAG` in `setup.bat` / `$imguiTag` in
`setup.ps1`. If your Visual Studio lives elsewhere, edit the `VCVARS` path near
the top of `build.bat`.

## Layout

```
src/metrics.h      data structs + query API
src/metrics.cpp    Win32/NT (RAM, commit, page file, processes) + DXGI/PDH (VRAM)
src/main.cpp       ImGui + DX11 host, theme, custom widgets, three pages
Logo.png           source logo (gold "R" / red "V" chip emblem)
app.ico / app.rc   multi-resolution window/taskbar icon, compiled by rc.exe
build.bat          one-shot MSVC build (rc + cl)
setup.bat/.ps1     dependency installer (vendors Dear ImGui at a pinned tag)
third_party/imgui  Dear ImGui (vendored via git, not committed)
```

## How the numbers are sourced

| Metric | API |
|---|---|
| Physical RAM, memory load | `GlobalMemoryStatusEx` |
| Commit total/limit/peak, pools, cache, counts | `GetPerformanceInfo` |
| Page file size + in-use | `NtQuerySystemInformation(SystemPageFileInformation)` |
| Per-process working set + private commit | `EnumProcesses` + `GetProcessMemoryInfo` (`PROCESS_MEMORY_COUNTERS_EX`) |
| Process names (incl. protected) | Toolhelp snapshot (`CreateToolhelp32Snapshot`) |
| VRAM totals + adapter enumeration | DXGI (`IDXGIAdapter3`, `DXGI_ADAPTER_DESC1`) |
| VRAM system-wide usage (dedicated/shared) | PDH `\GPU Adapter Memory(*)\...` |
| Per-process GPU memory | PDH `\GPU Process Memory(*)\...` |

## UI internals

The look is hand-built on top of Dear ImGui (no XAML): Segoe UI / Consolas fonts
loaded from the system, a custom gold/red theme, rounded "card" containers, and
`ImDrawList`-drawn ring gauges, sparklines and history graphs. Panels use
`ImGuiChildFlags_AlwaysUseWindowPadding` — borderless children ignore
`WindowPadding` without it.

## Ideas / next steps

- Right-click a process to end it or open its file location.
- Connected-monitor list per adapter (`IDXGIAdapter::EnumOutputs`).
- Minimize-to-tray for always-on monitoring.
- CSV/lightweight logging of history.

## License

MIT © 2026 Petra Thomas. See [LICENSE](LICENSE).
