// main.cpp - Dear ImGui (Win32 + DX11) front-end for the memory viewer.
// A hand-built "design system": Segoe UI fonts, a custom dark theme, rounded
// cards, ring gauges and live history graphs drawn with ImDrawList.
#include "metrics.h"

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include <d3d11.h>
#include <tchar.h>
#include <cstdio>
#include <cstring>
#include <cfloat>
#include <cctype>
#include <cmath>
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
// Fonts
// ---------------------------------------------------------------------------
static ImFont* g_fBody = nullptr;   // 17  Segoe UI
static ImFont* g_fH2   = nullptr;   // 20  Segoe UI Semibold  (headings, ring %)
static ImFont* g_fH1   = nullptr;   // 34  Segoe UI Semibold  (big values)
static ImFont* g_fMono = nullptr;   // 15  Consolas           (numeric columns)

// ImGui 1.92 removed single-arg PushFont; push at the size the font was loaded.
static void PushF(ImFont* f) { ImGui::PushFont(f, f->LegacySize); }

// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
namespace col {
    const ImVec4 bg      (0.055f, 0.060f, 0.070f, 1.00f);
    const ImVec4 side    (0.085f, 0.092f, 0.108f, 1.00f);
    const ImVec4 card    (0.105f, 0.113f, 0.130f, 1.00f);
    const ImVec4 cardHi  (0.140f, 0.150f, 0.172f, 1.00f);
    const ImVec4 line    (1.000f, 1.000f, 1.000f, 0.055f);
    const ImVec4 text    (0.902f, 0.914f, 0.937f, 1.00f);
    const ImVec4 dim     (0.560f, 0.585f, 0.640f, 1.00f);
    const ImVec4 faint   (0.380f, 0.400f, 0.450f, 1.00f);
    const ImVec4 blue    (0.290f, 0.600f, 1.000f, 1.00f);
    const ImVec4 green   (0.235f, 0.820f, 0.555f, 1.00f);
    const ImVec4 teal    (0.240f, 0.760f, 0.820f, 1.00f);
    const ImVec4 purple  (0.600f, 0.500f, 0.980f, 1.00f);
    const ImVec4 amber   (0.980f, 0.720f, 0.260f, 1.00f);
    const ImVec4 red     (0.960f, 0.360f, 0.390f, 1.00f);
}

// Shift an accent toward warning/critical as load climbs.
static ImVec4 LoadColor(ImVec4 base, double frac) {
    if (frac >= 0.92) return col::red;
    if (frac >= 0.80) return col::amber;
    return base;
}
static ImVec4 Alpha(ImVec4 c, float a) { c.w = a; return c; }

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------
static const char* FmtBytes(uint64_t b, char* buf, size_t n) {
    const double gb = 1073741824.0, mb = 1048576.0, kb = 1024.0;
    if      (b >= (uint64_t)gb) snprintf(buf, n, "%.2f GB", b / gb);
    else if (b >= (uint64_t)mb) snprintf(buf, n, "%.1f MB", b / mb);
    else if (b >= (uint64_t)kb) snprintf(buf, n, "%.0f KB", b / kb);
    else                        snprintf(buf, n, "%llu B", (unsigned long long)b);
    return buf;
}

// ---------------------------------------------------------------------------
// History ring buffer (one sample/sec, fixed window)
// ---------------------------------------------------------------------------
struct History {
    static const int CAP = 120;
    float v[CAP] = {};
    int   count  = 0;
    void push(float x) {
        if (count < CAP) v[count++] = x;
        else { memmove(v, v + 1, (CAP - 1) * sizeof(float)); v[CAP - 1] = x; }
    }
    float last() const { return count ? v[count - 1] : 0.0f; }
};
struct Histories { History ram, commit, vram; };

// ---------------------------------------------------------------------------
// Low-level custom widgets (ImDrawList)
// ---------------------------------------------------------------------------

// A filled area line inside an arbitrary rect.
static void DrawSeries(ImDrawList* dl, ImVec2 p0, ImVec2 p1,
                       const float* v, int count, ImVec4 c, float vmax) {
    if (count < 2) return;
    const float w = p1.x - p0.x, h = p1.y - p0.y;
    auto X = [&](int i){ return p0.x + w * (float)i / (float)(count - 1); };
    auto Y = [&](float val){ float f = vmax > 0 ? val / vmax : 0; f = f < 0 ? 0 : (f > 1 ? 1 : f);
                             return p1.y - f * h; };
    ImU32 fill = ImGui::GetColorU32(Alpha(c, 0.16f));
    ImU32 lcol = ImGui::GetColorU32(c);
    static std::vector<ImVec2> pts; pts.clear();
    for (int i = 0; i < count; ++i) {
        ImVec2 a(X(i), Y(v[i]));
        if (i) { ImVec2 pa(X(i-1), Y(v[i-1]));
                 dl->AddQuadFilled(ImVec2(pa.x, p1.y), pa, a, ImVec2(a.x, p1.y), fill); }
        pts.push_back(a);
    }
    dl->AddPolyline(pts.data(), count, lcol, 0, 2.0f);
    // bright dot on the latest sample
    dl->AddCircleFilled(pts.back(), 3.0f, lcol);
}

// Multi-series history graph card content.
static void HistoryGraph(const char* id, ImVec2 size, const History* h, const ImVec4* c,
                         const char** names, int n) {
    if (size.x < 8 || size.y < 8) return;
    ImGui::InvisibleButton(id, size);
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(Alpha(col::bg, 0.55f)), 8.0f);
    // horizontal grid at 25/50/75%
    ImU32 grid = ImGui::GetColorU32(col::line);
    for (int g = 1; g < 4; ++g) {
        float y = p1.y - (g * 0.25f) * size.y;
        dl->AddLine(ImVec2(p0.x + 6, y), ImVec2(p1.x - 6, y), grid, 1.0f);
    }
    ImVec2 in0(p0.x + 6, p0.y + 6), in1(p1.x - 6, p1.y - 6);
    for (int s = 0; s < n; ++s)
        DrawSeries(dl, in0, in1, h[s].v, h[s].count, c[s], 1.0f);
    // legend (top-left)
    float lx = p0.x + 12, ly = p0.y + 8;
    for (int s = 0; s < n; ++s) {
        char lbl[48]; snprintf(lbl, sizeof(lbl), "%s %.0f%%", names[s], h[s].last() * 100.0f);
        dl->AddCircleFilled(ImVec2(lx + 4, ly + 8), 4.0f, ImGui::GetColorU32(c[s]));
        dl->AddText(g_fBody, g_fBody->LegacySize, ImVec2(lx + 14, ly),
                    ImGui::GetColorU32(col::text), lbl);
        lx += g_fBody->CalcTextSizeA(g_fBody->LegacySize, FLT_MAX, 0, lbl).x + 32;
    }
}

// A minimal sparkline (background + single filled series, no legend/grid).
static void Sparkline(const char* id, ImVec2 size, const History& h, ImVec4 c) {
    if (size.x < 8 || size.y < 8) return;
    ImGui::InvisibleButton(id, size);
    ImVec2 p0 = ImGui::GetItemRectMin(), p1 = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(Alpha(col::bg, 0.55f)), 6.0f);
    DrawSeries(dl, ImVec2(p0.x + 5, p0.y + 5), ImVec2(p1.x - 5, p1.y - 5), h.v, h.count, c, 1.0f);
}

// Ring gauge: background ring + value arc + centered % text.
static void RingGauge(float diameter, float frac, ImVec4 c) {
    frac = frac < 0 ? 0 : (frac > 1 ? 1 : frac);
    ImGui::InvisibleButton("##ring", ImVec2(diameter, diameter));
    ImVec2 p0 = ImGui::GetItemRectMin();
    ImVec2 ctr(p0.x + diameter * 0.5f, p0.y + diameter * 0.5f);
    float  r  = diameter * 0.5f - 4.0f;
    float  th = r * 0.30f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float PI = 3.14159265358979323846f;
    const float a0 = -PI * 0.5f;
    dl->PathArcTo(ctr, r, a0, a0 + PI * 2.0f, 72);
    dl->PathStroke(ImGui::GetColorU32(Alpha(col::text, 0.10f)), 0, th);
    if (frac > 0.0001f) {
        dl->PathArcTo(ctr, r, a0, a0 + frac * PI * 2.0f, 72);
        dl->PathStroke(ImGui::GetColorU32(c), 0, th);
    }
    char pct[16]; snprintf(pct, sizeof(pct), "%.0f%%", frac * 100.0f);
    ImVec2 ts = g_fH2->CalcTextSizeA(g_fH2->LegacySize, FLT_MAX, 0, pct);
    dl->AddText(g_fH2, g_fH2->LegacySize, ImVec2(ctr.x - ts.x * 0.5f, ctr.y - ts.y * 0.5f),
                ImGui::GetColorU32(col::text), pct);
}

// A rounded card container. Call CardEnd() to close.
static bool CardBegin(const char* id, ImVec2 size, ImGuiChildFlags extra = 0) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::card);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    bool open = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | extra, ImGuiWindowFlags_NoScrollbar);
    return open;
}
static void CardEnd() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
}

// Small heading label inside a card.
static void CardTitle(const char* t) {
    PushF(g_fH2);
    ImGui::TextColored(col::text, "%s", t);
    ImGui::PopFont();
}

// ---------------------------------------------------------------------------
// KPI card: ring + label + value + sparkline
// ---------------------------------------------------------------------------
static void KpiCard(const char* id, const char* title, uint64_t used, uint64_t total,
                    double frac, ImVec4 accent, const History& hist, float cardW) {
    ImVec4 c = LoadColor(accent, frac);
    if (!CardBegin(id, ImVec2(cardW, 150))) { CardEnd(); return; }

    ImGui::BeginGroup();
    RingGauge(96.0f, (float)frac, c);
    ImGui::EndGroup();
    ImGui::SameLine(0, 16);

    ImGui::BeginGroup();
    CardTitle(title);
    char u[32], t[32];
    FmtBytes(used, u, sizeof(u)); FmtBytes(total, t, sizeof(t));
    PushF(g_fMono);
    ImGui::TextColored(col::text, "%s", u);
    ImGui::PopFont();
    PushF(g_fBody);
    ImGui::TextColored(col::dim, "of %s", t);
    ImGui::PopFont();
    ImGui::Spacing();
    float sw = ImGui::GetContentRegionAvail().x;
    Sparkline((std::string("spark") + id).c_str(), ImVec2(sw, 42), hist, c);
    ImGui::EndGroup();

    CardEnd();
}

// A compact stat chip (label + value) used in the info strip.
static void Chip(const char* label, const char* value, float w) {
    if (!CardBegin((std::string("chip") + label).c_str(), ImVec2(w, 66))) { CardEnd(); return; }
    PushF(g_fBody); ImGui::TextColored(col::dim, "%s", label); ImGui::PopFont();
    PushF(g_fH2);   ImGui::TextColored(col::text, "%s", value); ImGui::PopFont();
    CardEnd();
}

// A slim labelled usage bar (used on the GPU page).
static void UsageBar(const char* label, uint64_t used, uint64_t total, double frac, ImVec4 accent) {
    ImVec4 c = LoadColor(accent, frac);
    char u[32], t[32], overlay[80];
    FmtBytes(used, u, sizeof(u)); FmtBytes(total, t, sizeof(t));
    snprintf(overlay, sizeof(overlay), "%s / %s  (%.0f%%)", u, t, frac * 100.0);
    ImGui::TextColored(col::dim, "%s", label);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, c);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, Alpha(col::bg, 0.6f));
    ImGui::ProgressBar((float)frac, ImVec2(-FLT_MIN, 20), overlay);
    ImGui::PopStyleColor(2);
}

// ---------------------------------------------------------------------------
// GPU process classification (compositor / overlay double-counters)
// ---------------------------------------------------------------------------
enum class GpuProcKind { Normal, Compositor, Overlay };
static GpuProcKind ClassifyGpuProc(const std::string& name) {
    std::string n; n.reserve(name.size());
    for (char c : name) n += (char)tolower((unsigned char)c);
    if (n.rfind("dwm", 0) == 0)                 return GpuProcKind::Compositor;
    if (n.find("overlay") != std::string::npos) return GpuProcKind::Overlay;
    return GpuProcKind::Normal;
}

// ===========================================================================
// Pages
// ===========================================================================
static void PageDashboard(const SystemMemory& sys, const std::vector<AdapterVram>& gpus,
                          const Histories& H) {
    PushF(g_fH1);
    ImGui::TextColored(col::text, "Dashboard");
    ImGui::PopFont();
    ImGui::TextColored(col::dim, "Live system memory  \xc2\xb7  RAM, commit and VRAM");
    ImGui::Dummy(ImVec2(0, 6));

    // --- KPI row ---
    float gap = 14.0f;
    float cardW = (ImGui::GetContentRegionAvail().x - 2 * gap) / 3.0f;

    uint64_t vramUsed = 0, vramTot = 0; double vramFrac = 0;
    if (!gpus.empty() && gpus[0].hasUsage) {
        vramUsed = gpus[0].dedicatedUsage; vramTot = gpus[0].dedicatedTotal;
        vramFrac = vramTot ? (double)vramUsed / vramTot : 0;
    }

    KpiCard("kRam", "Physical RAM", sys.physUsed, sys.physTotal, sys.physPercent, col::blue, H.ram, cardW);
    ImGui::SameLine(0, gap);
    KpiCard("kCom", "Commit charge", sys.commitTotal, sys.commitLimit, sys.commitPercent, col::purple, H.commit, cardW);
    ImGui::SameLine(0, gap);
    KpiCard("kVram", "VRAM (dedicated)", vramUsed, vramTot, vramFrac, col::green, H.vram, cardW);

    ImGui::Dummy(ImVec2(0, 4));

    // --- History graph ---
    if (CardBegin("histCard", ImVec2(0, 220))) {
        CardTitle("History  \xc2\xb7  last 2 minutes");
        ImGui::Spacing();
        const History  hs[] = { H.ram, H.commit, H.vram };
        const ImVec4   cs[] = { col::blue, col::purple, col::green };
        const char*    ns[] = { "RAM", "Commit", "VRAM" };
        HistoryGraph("histG", ImGui::GetContentRegionAvail(), hs, cs, ns, 3);
    }
    CardEnd();

    ImGui::Dummy(ImVec2(0, 4));

    // --- Info chips ---
    char b0[32], b1[32], b2[32], b3[32], b4[32], b5[32];
    float cw = (ImGui::GetContentRegionAvail().x - 5 * 10.0f) / 6.0f;
    Chip("Available",  FmtBytes(sys.physAvail, b0, sizeof(b0)), cw); ImGui::SameLine(0, 10);
    Chip("Cached",     FmtBytes(sys.systemCache, b1, sizeof(b1)), cw); ImGui::SameLine(0, 10);
    Chip("Paged pool", FmtBytes(sys.kernelPaged, b2, sizeof(b2)), cw); ImGui::SameLine(0, 10);
    snprintf(b3, sizeof(b3), "%u", sys.processCount);
    Chip("Processes",  b3, cw); ImGui::SameLine(0, 10);
    snprintf(b4, sizeof(b4), "%u", sys.threadCount);
    Chip("Threads",    b4, cw); ImGui::SameLine(0, 10);
    snprintf(b5, sizeof(b5), "%u", sys.handleCount);
    Chip("Handles",    b5, cw);
}

static void PageProcesses(const std::vector<ProcessMemory>& procs, uint64_t accessiblePrivate) {
    PushF(g_fH1); ImGui::TextColored(col::text, "Processes"); ImGui::PopFont();
    char sum[32];
    ImGui::TextColored(col::dim, "Sorted by private commit \xe2\x80\x94 the page-file eaters  \xc2\xb7  %s across %d",
                       FmtBytes(accessiblePrivate, sum, sizeof(sum)), (int)procs.size());
    ImGui::Dummy(ImVec2(0, 6));

    uint64_t maxPriv = procs.empty() ? 1 : procs.front().privateBytes;
    if (maxPriv == 0) maxPriv = 1;

    if (!CardBegin("procCard", ImVec2(0, 0))) { CardEnd(); return; }
    ImGuiTableFlags f = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_Resizable | ImGuiTableFlags_Sortable |
                        ImGuiTableFlags_SortTristate | ImGuiTableFlags_BordersInnerH;
    if (ImGui::BeginTable("procs", 4, f)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("PID",            ImGuiTableColumnFlags_WidthFixed, 64);
        ImGui::TableSetupColumn("Name",           ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Private commit", ImGuiTableColumnFlags_WidthFixed |
                                ImGuiTableColumnFlags_DefaultSort |
                                ImGuiTableColumnFlags_PreferSortDescending, 150);
        ImGui::TableSetupColumn("Working set",    ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableHeadersRow();

        static std::vector<const ProcessMemory*> view; view.clear();
        for (auto& p : procs) view.push_back(&p);
        if (ImGuiTableSortSpecs* ss = ImGui::TableGetSortSpecs()) {
            if (ss->SpecsCount > 0) {
                const ImGuiTableColumnSortSpecs& s = ss->Specs[0];
                bool asc = s.SortDirection == ImGuiSortDirection_Ascending;
                std::sort(view.begin(), view.end(), [&](const ProcessMemory* x, const ProcessMemory* y){
                    long long d = 0;
                    switch (s.ColumnIndex) {
                        case 0: d = (long long)x->pid - (long long)y->pid; break;
                        case 1: d = x->name.compare(y->name); break;
                        case 2: d = (x->privateBytes>y->privateBytes)?1:(x->privateBytes<y->privateBytes?-1:0); break;
                        case 3: d = (x->workingSet>y->workingSet)?1:(x->workingSet<y->workingSet?-1:0); break;
                    }
                    return asc ? d < 0 : d > 0;
                });
            }
        }

        char a[32], b[32];
        for (const ProcessMemory* p : view) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            PushF(g_fMono); ImGui::TextColored(col::dim, "%u", p->pid); ImGui::PopFont();
            ImGui::TableNextColumn();
            if (!p->accessible) ImGui::TextColored(col::faint, "%s", p->name.c_str());
            else                ImGui::TextColored(col::text, "%s", p->name.c_str());
            // Private commit cell with a faint proportional heat bar behind it.
            ImGui::TableNextColumn();
            {
                ImVec2 cp = ImGui::GetCursorScreenPos();
                float  cw = ImGui::GetContentRegionAvail().x;
                float  lh = ImGui::GetTextLineHeight();
                float  fr = (float)((double)p->privateBytes / (double)maxPriv);
                ImGui::GetWindowDrawList()->AddRectFilled(
                    cp, ImVec2(cp.x + cw * fr, cp.y + lh),
                    ImGui::GetColorU32(Alpha(col::blue, 0.18f)), 3.0f);
                PushF(g_fMono); ImGui::TextColored(col::text, "%s", FmtBytes(p->privateBytes, a, sizeof(a))); ImGui::PopFont();
            }
            ImGui::TableNextColumn();
            PushF(g_fMono); ImGui::TextColored(col::dim, "%s", FmtBytes(p->workingSet, b, sizeof(b))); ImGui::PopFont();
        }
        ImGui::EndTable();
    }
    CardEnd();
}

static void PageGpu(const std::vector<AdapterVram>& gpus, const std::vector<ProcessVram>& procVram) {
    PushF(g_fH1); ImGui::TextColored(col::text, "GPU Memory"); ImGui::PopFont();
    ImGui::TextColored(col::dim, "What each process is holding in VRAM");
    ImGui::Dummy(ImVec2(0, 6));
    char a[32], b[32];

    // Resident headline card (auto-sizes to its content so the table below fits)
    if (CardBegin("resCard", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY)) {
        CardTitle("Physically resident on the GPU  \xc2\xb7  the real number");
        ImGui::Spacing();
        if (gpus.empty()) ImGui::TextColored(col::dim, "No GPU adapter reported.");
        for (size_t i = 0; i < gpus.size(); ++i) {
            const AdapterVram& g = gpus[i];
            ImGui::PushID((int)i);
            ImGui::TextColored(col::text, "%s", g.name.c_str());
            if (g.hasUsage) {
                double df = g.dedicatedTotal ? (double)g.dedicatedUsage / g.dedicatedTotal : 0;
                double sf = g.sharedTotal    ? (double)g.sharedUsage    / g.sharedTotal    : 0;
                UsageBar("On-card VRAM (resident)",      g.dedicatedUsage, g.dedicatedTotal, df, col::green);
                UsageBar("Shared system RAM (resident)", g.sharedUsage,    g.sharedTotal,    sf, col::teal);
            }
            ImGui::PopID();
        }
    }
    CardEnd();

    ImGui::Dummy(ImVec2(0, 4));

    if (CardBegin("gpuProcCard", ImVec2(0, 0))) {
        CardTitle("Per-process GPU memory  \xc2\xb7  committed, overlaps, not physical");
        PushF(g_fBody);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(col::dim,
            "Committed (virtualized) allocations \xe2\x80\x94 they double-count shared surfaces and "
            "include memory that isn't resident, so they exceed physical VRAM. Rank, don't total.");
        ImGui::PopTextWrapPos();
        ImGui::PopFont();

        for (const ProcessVram& p : procVram)
            if (ClassifyGpuProc(p.name) == GpuProcKind::Compositor) {
                PushF(g_fBody);
                ImGui::TextColored(col::amber, "%s reports %s", p.name.c_str(), FmtBytes(p.dedicated, a, sizeof(a)));
                ImGui::SameLine();
                ImGui::TextColored(col::dim, "\xe2\x80\x94 the compositor: sum of every visible window's surface (double-counted).");
                ImGui::PopFont();
                break;
            }
        ImGui::Spacing();

        if (procVram.empty()) {
            ImGui::TextColored(col::dim, "No per-process GPU memory reported (needs WDDM 2.0+).");
        } else {
            ImGuiTableFlags f = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                                ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerH;
            if (ImGui::BeginTable("gpuprocs", 5, f)) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("PID",       ImGuiTableColumnFlags_WidthFixed, 64);
                ImGui::TableSetupColumn("Name",      ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("Committed", ImGuiTableColumnFlags_WidthFixed, 110);
                ImGui::TableSetupColumn("Shared",    ImGuiTableColumnFlags_WidthFixed, 100);
                ImGui::TableSetupColumn("Note",      ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableHeadersRow();
                const ImU32 tint = ImGui::GetColorU32(Alpha(col::amber, 0.10f));
                for (const ProcessVram& p : procVram) {
                    GpuProcKind k = ClassifyGpuProc(p.name);
                    ImGui::TableNextRow();
                    if (k != GpuProcKind::Normal) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, tint);
                    ImGui::TableNextColumn();
                    PushF(g_fMono); ImGui::TextColored(col::dim, "%u", p.pid); ImGui::PopFont();
                    ImGui::TableNextColumn();
                    ImGui::TextColored(k != GpuProcKind::Normal ? col::amber : col::text, "%s", p.name.c_str());
                    ImGui::TableNextColumn();
                    PushF(g_fMono); ImGui::TextColored(col::text, "%s", FmtBytes(p.dedicated, a, sizeof(a))); ImGui::PopFont();
                    ImGui::TableNextColumn();
                    PushF(g_fMono);
                    if (p.shared) ImGui::TextColored(col::dim, "%s", FmtBytes(p.shared, b, sizeof(b)));
                    else          ImGui::TextColored(col::faint, "-");
                    ImGui::PopFont();
                    ImGui::TableNextColumn();
                    if (k == GpuProcKind::Compositor) ImGui::TextColored(col::faint, "compositor \xc2\xb7 sums all windows");
                    else if (k == GpuProcKind::Overlay) ImGui::TextColored(col::faint, "overlay \xc2\xb7 over-reports");
                }
                ImGui::EndTable();
            }
        }
    }
    CardEnd();
}

// ---------------------------------------------------------------------------
// Sidebar + shell
// ---------------------------------------------------------------------------
static void NavItem(const char* label, int idx, int* page) {
    bool sel = (*page == idx);
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_Header,        Alpha(col::blue, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Alpha(col::text, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  Alpha(col::blue, 0.22f));
    ImGui::PushStyleColor(ImGuiCol_Text, sel ? col::text : col::dim);
    if (ImGui::Selectable(label, sel, 0, ImVec2(0, 34))) *page = idx;
    ImGui::PopStyleColor(4);
    if (sel) { // accent bar on the left edge
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(ImVec2(p0.x - 8, p0.y + 6), ImVec2(p0.x - 5, p0.y + 28),
                          ImGui::GetColorU32(col::blue), 2.0f);
    }
}

static void DrawUI(const SystemMemory& sys, const std::vector<AdapterVram>& gpus,
                   const std::vector<ProcessMemory>& procs, uint64_t accessiblePrivate,
                   const std::vector<ProcessVram>& procVram, const Histories& H) {
    static int page = 0;

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("RVMemViewer", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::PopStyleVar();

    // ---- Sidebar ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::side);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22, 24));
    ImGui::BeginChild("sidebar", ImVec2(202, 0), 0, ImGuiWindowFlags_NoScrollbar);
    PushF(g_fH2);
    ImGui::TextColored(col::blue, "RV");
    ImGui::SameLine(0, 6);
    ImGui::TextColored(col::text, "MEM Viewer");
    ImGui::PopFont();
    ImGui::TextColored(col::faint, "RAM \xc2\xb7 VRAM \xc2\xb7 commit");
    ImGui::Dummy(ImVec2(0, 18));
    NavItem("  Dashboard", 0, &page);
    NavItem("  Processes", 1, &page);
    NavItem("  GPU Memory", 2, &page);

    // pinned footer: live RAM/commit mini readout
    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 78);
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, 4));
    PushF(g_fBody);
    ImGui::TextColored(col::dim, "RAM %.0f%%   Commit %.0f%%",
                       sys.physPercent * 100.0, sys.commitPercent * 100.0);
    ImGui::TextColored(col::faint, "%u processes", sys.processCount);
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::SameLine(0, 0);

    // ---- Content ----
    // Responsive side padding: grows with width so it reads well both at the
    // default size and maximized on an ultra-wide monitor.
    float contentW = vp->WorkSize.x - 202.0f;
    float padX = contentW * 0.030f;
    padX = padX < 34.0f ? 34.0f : (padX > 140.0f ? 140.0f : padX);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::bg);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padX, 30));
    ImGui::BeginChild("content", ImVec2(0, 0), 0);
    switch (page) {
        case 0: PageDashboard(sys, gpus, H); break;
        case 1: PageProcesses(procs, accessiblePrivate); break;
        case 2: PageGpu(gpus, procVram); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::End();
}

// ---------------------------------------------------------------------------
// Theme + fonts
// ---------------------------------------------------------------------------
static void ApplyTheme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 0.0f;
    s.ChildRounding     = 10.0f;
    s.FrameRounding     = 6.0f;
    s.GrabRounding      = 6.0f;
    s.PopupRounding     = 6.0f;
    s.ScrollbarRounding = 8.0f;
    s.TabRounding       = 6.0f;
    s.FrameBorderSize   = 0.0f;
    s.ChildBorderSize   = 1.0f;
    s.WindowPadding     = ImVec2(16, 16);
    s.FramePadding      = ImVec2(10, 6);
    s.ItemSpacing       = ImVec2(10, 8);
    s.CellPadding       = ImVec2(10, 6);
    s.ScrollbarSize     = 12.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]        = col::bg;
    c[ImGuiCol_ChildBg]         = col::card;
    c[ImGuiCol_PopupBg]         = col::side;
    c[ImGuiCol_Border]          = col::line;
    c[ImGuiCol_Text]            = col::text;
    c[ImGuiCol_TextDisabled]    = col::dim;
    c[ImGuiCol_FrameBg]         = Alpha(col::text, 0.05f);
    c[ImGuiCol_FrameBgHovered]  = Alpha(col::text, 0.09f);
    c[ImGuiCol_FrameBgActive]   = Alpha(col::text, 0.12f);
    c[ImGuiCol_TableHeaderBg]   = Alpha(col::text, 0.04f);
    c[ImGuiCol_TableRowBg]      = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt]   = Alpha(col::text, 0.025f);
    c[ImGuiCol_TableBorderLight]= col::line;
    c[ImGuiCol_TableBorderStrong]=col::line;
    c[ImGuiCol_Header]          = Alpha(col::blue, 0.16f);
    c[ImGuiCol_HeaderHovered]   = Alpha(col::text, 0.06f);
    c[ImGuiCol_HeaderActive]    = Alpha(col::blue, 0.22f);
    c[ImGuiCol_Button]          = Alpha(col::text, 0.06f);
    c[ImGuiCol_ButtonHovered]   = Alpha(col::text, 0.10f);
    c[ImGuiCol_ButtonActive]    = Alpha(col::text, 0.14f);
    c[ImGuiCol_ScrollbarBg]     = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab]   = Alpha(col::text, 0.14f);
    c[ImGuiCol_ScrollbarGrabHovered] = Alpha(col::text, 0.22f);
    c[ImGuiCol_Separator]       = col::line;
    c[ImGuiCol_PlotHistogram]   = col::blue;
}

static bool FileExists(const char* p) {
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static ImFont* AddTTF(ImGuiIO& io, const char* path, float size) {
    return FileExists(path) ? io.Fonts->AddFontFromFileTTF(path, size) : nullptr;
}
static void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const char* sui = "C:\\Windows\\Fonts\\segoeui.ttf";
    const char* ssb = "C:\\Windows\\Fonts\\segoeuisb.ttf";
    const char* con = "C:\\Windows\\Fonts\\consola.ttf";
    g_fBody = AddTTF(io, sui, 17.0f);
    if (!g_fBody) g_fBody = io.Fonts->AddFontDefault();
    g_fH2 = AddTTF(io, ssb, 20.0f); if (!g_fH2) g_fH2 = AddTTF(io, sui, 20.0f); if (!g_fH2) g_fH2 = g_fBody;
    g_fH1 = AddTTF(io, ssb, 34.0f); if (!g_fH1) g_fH1 = AddTTF(io, sui, 34.0f); if (!g_fH1) g_fH1 = g_fBody;
    g_fMono = AddTTF(io, con, 15.0f); if (!g_fMono) g_fMono = g_fBody;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, hInstance,
                       nullptr, nullptr, nullptr, nullptr, L"RVMemViewerWnd", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"RV MEM Viewer",
                              WS_OVERLAPPEDWINDOW, 100, 100, 1120, 800,
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
    io.IniFilename = nullptr;
    LoadFonts();
    ApplyTheme();

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    InitVram();

    SystemMemory sys{};
    std::vector<AdapterVram>   gpus;
    std::vector<ProcessMemory> procs;
    std::vector<ProcessVram>   procVram;
    Histories                  H;
    uint64_t accessiblePrivate = 0;
    double   sinceRefresh = 1e9;

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
            Sleep(80); continue;
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
            QueryProcessVram(procVram);
            QueryProcesses(procs, accessiblePrivate);
            H.ram.push((float)sys.physPercent);
            H.commit.push((float)sys.commitPercent);
            float vf = 0;
            if (!gpus.empty() && gpus[0].hasUsage && gpus[0].dedicatedTotal)
                vf = (float)((double)gpus[0].dedicatedUsage / gpus[0].dedicatedTotal);
            H.vram.push(vf);
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        PushF(g_fBody);
        DrawUI(sys, gpus, procs, accessiblePrivate, procVram, H);
        ImGui::PopFont();
        ImGui::Render();

        const float clear[4] = { col::bg.x, col::bg.y, col::bg.z, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_swapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
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
    if (hr == DXGI_ERROR_UNSUPPORTED)
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
    if (back) { g_pd3dDevice->CreateRenderTargetView(back, nullptr, &g_mainRTV); back->Release(); }
}

void CleanupRenderTarget() {
    if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; }
}

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) { g_resizeW = LOWORD(lParam); g_resizeH = HIWORD(lParam); }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
