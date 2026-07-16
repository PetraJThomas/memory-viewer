// main.cpp - Dear ImGui (Win32 + DirectX 11) front-end for the memory viewer.
#include "metrics.h"

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include <d3d11.h>
#include <tchar.h>
#include <cstdio>
#include <cfloat>
#include <vector>
#include <algorithm>

#pragma comment(lib, "d3d11.lib")

// ---------------------------------------------------------------------------
// D3D globals
// ---------------------------------------------------------------------------
static ID3D11Device*           g_pd3dDevice        = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*         g_pSwapChain        = nullptr;
static ID3D11RenderTargetView* g_mainRTV           = nullptr;
static bool                    g_swapChainOccluded  = false;
static UINT                    g_resizeW = 0, g_resizeH = 0;

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ---------------------------------------------------------------------------
// Formatting + UI helpers
// ---------------------------------------------------------------------------
static const char* FmtBytes(uint64_t b, char* buf, size_t n) {
    const double gb = 1024.0 * 1024.0 * 1024.0;
    const double mb = 1024.0 * 1024.0;
    const double kb = 1024.0;
    if      (b >= (uint64_t)(gb)) snprintf(buf, n, "%.2f GB", b / gb);
    else if (b >= (uint64_t)(mb)) snprintf(buf, n, "%.1f MB", b / mb);
    else if (b >= (uint64_t)(kb)) snprintf(buf, n, "%.0f KB", b / kb);
    else                          snprintf(buf, n, "%llu B", (unsigned long long)b);
    return buf;
}

// A labelled usage bar: "12.4 / 32.0 GB" overlaid, colour shifts with load.
static void UsageBar(const char* label, uint64_t used, uint64_t total, double frac) {
    ImVec4 col = frac < 0.75 ? ImVec4(0.26f, 0.59f, 0.98f, 1.0f)   // blue
              : frac < 0.90 ? ImVec4(0.95f, 0.65f, 0.15f, 1.0f)   // amber
                            : ImVec4(0.90f, 0.25f, 0.25f, 1.0f);  // red
    char u[32], t[32], overlay[80];
    FmtBytes(used, u, sizeof(u));
    FmtBytes(total, t, sizeof(t));
    snprintf(overlay, sizeof(overlay), "%s / %s  (%.0f%%)", u, t, frac * 100.0);

    ImGui::TextUnformatted(label);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
    ImGui::ProgressBar((float)frac, ImVec2(-FLT_MIN, 22), overlay);
    ImGui::PopStyleColor();
}

// ---------------------------------------------------------------------------
// The actual viewer UI (one full-window panel)
// ---------------------------------------------------------------------------
static void DrawUI(const SystemMemory& sys,
                   const std::vector<AdapterVram>& gpus,
                   const std::vector<ProcessMemory>& procs,
                   uint64_t accessiblePrivate) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("MemoryViewer", nullptr, flags);

    char a[32], b[32];

    // ---- System RAM + commit ------------------------------------------------
    ImGui::SeparatorText("System Memory");
    UsageBar("Physical RAM", sys.physUsed, sys.physTotal, sys.physPercent);
    ImGui::Spacing();
    UsageBar("Commit charge  (RAM + page file)", sys.commitTotal, sys.commitLimit, sys.commitPercent);
    ImGui::TextDisabled("This is the number your page file backs. Limit = RAM + all page files.");

    ImGui::Spacing();
    if (ImGui::BeginTable("sysdetail", 4, ImGuiTableFlags_SizingStretchSame)) {
        auto cell = [&](const char* k, const char* v) {
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", k); ImGui::SameLine(); ImGui::TextUnformatted(v);
        };
        cell("Available", FmtBytes(sys.physAvail, a, sizeof(a)));
        cell("Cached",    FmtBytes(sys.systemCache, b, sizeof(b)));
        cell("Commit peak", FmtBytes(sys.commitPeak, a, sizeof(a)));
        { char tmp[32]; cell("Paged pool", FmtBytes(sys.kernelPaged, tmp, sizeof(tmp))); }
        { char tmp[32]; cell("Non-paged pool", FmtBytes(sys.kernelNonpaged, tmp, sizeof(tmp))); }
        { char tmp[48]; snprintf(tmp, sizeof(tmp), "%u", sys.processCount); cell("Processes", tmp); }
        { char tmp[48]; snprintf(tmp, sizeof(tmp), "%u", sys.threadCount);  cell("Threads", tmp); }
        { char tmp[48]; snprintf(tmp, sizeof(tmp), "%u", sys.handleCount);  cell("Handles", tmp); }
        ImGui::EndTable();
    }

    // ---- VRAM ---------------------------------------------------------------
    ImGui::SeparatorText("GPU Memory (VRAM)");
    if (gpus.empty()) {
        ImGui::TextDisabled("No DXGI adapter reported.");
    }
    for (size_t i = 0; i < gpus.size(); ++i) {
        const AdapterVram& g = gpus[i];
        ImGui::PushID((int)i);
        ImGui::Text("%s", g.name.c_str());
        if (g.hasUsage) {
            double dfrac = g.dedicatedTotal ? (double)g.dedicatedUsage / g.dedicatedTotal : 0.0;
            double sfrac = g.sharedTotal    ? (double)g.sharedUsage    / g.sharedTotal    : 0.0;
            UsageBar("  Dedicated (on-board VRAM)", g.dedicatedUsage, g.dedicatedTotal, dfrac);
            UsageBar("  Shared (system RAM)",       g.sharedUsage,    g.sharedTotal,    sfrac);
        } else {
            ImGui::TextDisabled("  Live usage unavailable; dedicated total: %s",
                                FmtBytes(g.dedicatedTotal, a, sizeof(a)));
        }
        ImGui::PopID();
        ImGui::Spacing();
    }

    // ---- Process table ------------------------------------------------------
    ImGui::SeparatorText("Processes  (sorted by private commit \xe2\x80\x94 the page-file eaters)");
    {
        char sum[32];
        ImGui::TextDisabled("Readable private commit total: %s across %d processes",
                            FmtBytes(accessiblePrivate, sum, sizeof(sum)), (int)procs.size());
    }

    ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                             ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                             ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate;
    if (ImGui::BeginTable("procs", 4, tflags, ImVec2(0, 0))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("PID",            ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Name",           ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Private commit", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_PreferSortDescending, 130);
        ImGui::TableSetupColumn("Working set",    ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableHeadersRow();

        // Re-sort a local copy according to the table's sort specs.
        static std::vector<const ProcessMemory*> view;
        view.clear();
        view.reserve(procs.size());
        for (auto& p : procs) view.push_back(&p);

        if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs()) {
            if (ss->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& s = ss->Specs[0];
                bool asc = s.SortDirection == ImGuiSortDirection_Ascending;
                std::sort(view.begin(), view.end(), [&](const ProcessMemory* x, const ProcessMemory* y) {
                    long long d = 0;
                    switch (s.ColumnIndex) {
                        case 0: d = (long long)x->pid - (long long)y->pid; break;
                        case 1: d = x->name.compare(y->name); break;
                        case 2: d = (x->privateBytes > y->privateBytes) ? 1 : (x->privateBytes < y->privateBytes ? -1 : 0); break;
                        case 3: d = (x->workingSet   > y->workingSet)   ? 1 : (x->workingSet   < y->workingSet   ? -1 : 0); break;
                    }
                    return asc ? d < 0 : d > 0;
                });
            }
        }

        for (const ProcessMemory* p : view) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%u", p->pid);
            ImGui::TableNextColumn();
            if (!p->accessible) ImGui::TextDisabled("%s", p->name.c_str());
            else                ImGui::TextUnformatted(p->name.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(FmtBytes(p->privateBytes, a, sizeof(a)));
            ImGui::TableNextColumn(); ImGui::TextUnformatted(FmtBytes(p->workingSet,   b, sizeof(b)));
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInstance,
                       nullptr, nullptr, nullptr, nullptr, L"MemoryViewerWnd", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Memory & VRAM Viewer",
                              WS_OVERLAPPEDWINDOW, 100, 100, 900, 760,
                              nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // don't litter an .ini file; stay self-contained
    ImGui::StyleColorsDark();
    io.FontGlobalScale = 1.15f;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    InitVram();

    // Data refreshed on a 1s cadence; UI still renders smoothly in between.
    SystemMemory sys{};
    std::vector<AdapterVram>   gpus;
    std::vector<ProcessMemory> procs;
    uint64_t accessiblePrivate = 0;
    double   sinceRefresh = 1e9; // force immediate first refresh

    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        if (g_swapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            Sleep(80);
            continue;
        }
        g_swapChainOccluded = false;

        if (g_resizeW != 0 && g_resizeH != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRenderTarget();
        }

        sinceRefresh += io.DeltaTime;
        if (sinceRefresh >= 1.0) {
            sinceRefresh = 0.0;
            QuerySystemMemory(sys);
            QueryVram(gpus);
            QueryProcesses(procs, accessiblePrivate);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUI(sys, gpus, procs, accessiblePrivate);
        ImGui::Render();

        const float clear[4] = { 0.09f, 0.09f, 0.11f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0); // vsync: caps GPU use
        g_swapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);

        // Ease off further when we're not the foreground window.
        if (GetForegroundWindow() != hwnd) Sleep(120);
    }

    ShutdownVram();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

// ---------------------------------------------------------------------------
// D3D plumbing
// ---------------------------------------------------------------------------
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT flags = 0;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                     flags, levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                     &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (hr == DXGI_ERROR_UNSUPPORTED) // fall back to WARP
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                     flags, levels, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
                     &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_pd3dDevice->CreateRenderTargetView(back, nullptr, &g_mainRTV);
        back->Release();
    }
}

void CleanupRenderTarget() {
    if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) { g_resizeW = LOWORD(lParam); g_resizeH = HIWORD(lParam); }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0; // no ALT menu
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
