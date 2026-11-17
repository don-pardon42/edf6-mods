// EDFStats in-game overlay (included by edfstats.cpp).
// Hooks IDXGISwapChain::Present / ResizeBuffers (found through a throw-away device) and draws with
// Dear ImGui (DX11 + Win32 backends). Display only: the overlay never takes mouse or keyboard input,
// it only reads its own hotkeys, so the game controls stay untouched.
//   live panel (F6): your damage, current and peak DPS, kills, and the other players' damage
//   scoreboard (F7): full table; opens by itself on the game's mission result screen
#include <d3d11.h>
#include <dxgi.h>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#pragma comment(lib, "d3d11.lib")

typedef HRESULT(__stdcall *PresentFn)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT(__stdcall *ResizeFn)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PresentFn oPresent; static ResizeFn oResize;
static ID3D11Device *g_dev; static ID3D11DeviceContext *g_ctx; static ID3D11RenderTargetView *g_rtv;
static HWND g_hwnd; static bool g_imgui, g_ovDead;
static int g_keyLive = VK_F6, g_keyBoard = VK_F7, g_overlayOn = 1;
static bool g_showLive = true, g_showBoard = false, g_kLive, g_kBoard;
static float g_uiScale = 1.0f, g_uiUser = 1.0f;   // g_uiUser: OverlayScale from the ini (percent)

static std::string NumSp(double v) {       // 12345.6 -> "12 346" (plain space, always in the font)
    char b[32]; snprintf(b, sizeof b, "%.0f", v);
    std::string s(b), o; int n = 0;
    for (int i = (int)s.size() - 1; i >= 0; --i) {
        o.insert(o.begin(), s[i]);
        if (isdigit((unsigned char)s[i]) && ++n % 3 == 0 && i > 0 && isdigit((unsigned char)s[i - 1])) o.insert(o.begin(), ' ');
    }
    return o;
}

static void OverlayStyle() {
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 4; st.FrameRounding = 3; st.WindowBorderSize = 1; st.WindowPadding = ImVec2(12, 10);
    st.CellPadding = ImVec2(8, 3); st.ItemSpacing = ImVec2(8, 4);
    ImVec4 *c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.07f, 0.06f, 0.86f);
    c[ImGuiCol_Border] = ImVec4(0.89f, 0.73f, 0.23f, 0.55f);
    c[ImGuiCol_Text] = ImVec4(0.91f, 0.94f, 0.91f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.56f, 0.64f, 0.60f, 1.00f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.10f, 0.14f, 0.12f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.17f, 0.23f, 0.20f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.24f, 0.32f, 0.28f, 1.00f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.03f);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.89f, 0.73f, 0.23f, 1.00f);
    c[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.19f, 0.17f, 1.00f);
}
static const ImVec4 GOLD(0.89f, 0.73f, 0.23f, 1), GREEN(0.37f, 0.82f, 0.55f, 1), DIM(0.56f, 0.64f, 0.60f, 1), RED(0.88f, 0.42f, 0.33f, 1);

static bool OverlayInit(IDXGISwapChain *sc) {
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void **)&g_dev)) || !g_dev) { Log("overlay: not a D3D11 swap chain, off"); return false; }
    g_dev->GetImmediateContext(&g_ctx);
    DXGI_SWAP_CHAIN_DESC d; sc->GetDesc(&d);
    g_hwnd = d.OutputWindow;
    RECT rc; GetClientRect(g_hwnd, &rc);
    float h = (float)std::max<LONG>(rc.bottom - rc.top, 600);
    g_uiScale = std::max(0.8f, h / 1080.0f) * g_uiUser;
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange | ImGuiConfigFlags_NoKeyboard;
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x0400, 0x052F, 0x2000, 0x206F, 0x2116, 0x2116, 0 };
    char win[MAX_PATH]; GetWindowsDirectoryA(win, MAX_PATH);
    std::string fontDir = std::string(win) + "\\Fonts\\";
    float px = 17.0f * g_uiScale;
    ImFont *f = nullptr;
    for (const char *name : { "segoeui.ttf", "arial.ttf", "tahoma.ttf" }) {
        std::string path = fontDir + name;
        if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES && (f = io.Fonts->AddFontFromFileTTF(path.c_str(), px, nullptr, ranges))) break;
    }
    if (!f) { Log("overlay: no system font with Cyrillic found, using the built-in one"); io.Fonts->AddFontDefault(); }
    std::string bold = fontDir + "segoeuib.ttf";      // second font: headings and big numbers
    if (GetFileAttributesA(bold.c_str()) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(bold.c_str(), px * 1.25f, nullptr, ranges);
    OverlayStyle();
    ImGui::GetStyle().ScaleAllSizes(g_uiScale);
    if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX11_Init(g_dev, g_ctx)) { Log("overlay: ImGui init failed, off"); return false; }
    Log("overlay ready (%ldx%ld, scale %.2f). F6 = live panel, F7 = scoreboard", rc.right - rc.left, rc.bottom - rc.top, g_uiScale);
    return true;
}
static void MakeRtv(IDXGISwapChain *sc) {
    ID3D11Texture2D *bb = nullptr;
    if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&bb)) && bb) {
        g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
        bb->Release();
    }
}
static ImFont *Bold() { ImGuiIO &io = ImGui::GetIO(); return io.Fonts->Fonts.Size > 1 ? io.Fonts->Fonts[1] : io.Fonts->Fonts[0]; }
static const char *RowName(const Row &r) { return r.s.name[0] ? r.s.name : r.s.cls; }

static void DrawLive(const LiveView &v) {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16 * g_uiScale, 120 * g_uiScale), ImGuiCond_Always, ImVec2(1, 0));
    ImGui::SetNextWindowBgAlpha(0.70f);
    ImGui::Begin("##live", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
    const Row *me = nullptr; for (auto &r : v.rows) if (r.local) me = &r;
    if (me) {
        ImGui::PushFont(Bold()); ImGui::TextColored(GOLD, "%s", NumSp(me->s.dmg).c_str()); ImGui::PopFont();
        ImGui::SameLine(); ImGui::TextColored(DIM, L_(u8"урон", u8"damage"));
        ImGui::Text(u8"DPS  %s", NumSp(v.dps1).c_str()); ImGui::SameLine(); ImGui::TextColored(DIM, L_(u8"(5 с: %s)", u8"(5 s: %s)"), NumSp(v.dps5).c_str());
        ImGui::TextColored(DIM, L_(u8"пик %s", u8"peak %s"), NumSp(me->s.peak1).c_str());
        ImGui::Text(L_(u8"Убийства %u", u8"Kills %u"), me->s.kills);
        if (me->s.parts) { ImGui::SameLine(); ImGui::TextColored(DIM, L_(u8"  частей %u", u8"  parts %u"), me->s.parts); }
    } else ImGui::TextColored(DIM, L_(u8"EDFStats: ждёт начала боя", u8"EDFStats: waiting for the fight"));
    if (v.rows.size() > 1) {
        ImGui::Separator();
        double top = 1; for (auto &r : v.rows) top = std::max(top, (double)r.s.dmg);
        for (auto &r : v.rows) {
            ImGui::TextColored(r.local ? GREEN : ImVec4(0.91f, 0.94f, 0.91f, 1), "%s", RowName(r));
            ImGui::SameLine(170 * g_uiScale);
            ImGui::Text("%s", NumSp(r.s.dmg).c_str());
            ImGui::SameLine(250 * g_uiScale);
            ImGui::TextColored(DIM, "%u", r.s.kills);
            ImGui::ProgressBar((float)(r.s.dmg / top), ImVec2(260 * g_uiScale, 3 * g_uiScale), "");
        }
    }
    ImGui::End();
}

static void DrawBoard(const LiveView &v) {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.12f), ImGuiCond_Always, ImVec2(0.5f, 0));
    ImGui::SetNextWindowBgAlpha(0.90f);
    ImGui::Begin("##board", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                 ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PushFont(Bold());
    ImGui::TextColored(GOLD, v.ended ? L_(u8"ИТОГИ МИССИИ", u8"MISSION RESULTS") : L_(u8"СТАТИСТИКА МИССИИ", u8"MISSION STATS"));
    ImGui::PopFont();
    ImGui::SameLine(); ImGui::TextColored(DIM, L_(u8"  %d:%02d  ·  F7 — скрыть", u8"  %d:%02d  ·  F7 — hide"), (int)v.secs / 60, (int)v.secs % 60);
    if (v.rows.empty()) { ImGui::TextColored(DIM, L_(u8"Пока нет данных.", u8"No data yet.")); ImGui::End(); return; }

    // awards (the same ones as in the HTML report, short form)
    struct Aw { const char *t; double (*f)(const Summary &); };
    static const Aw aws[] = {
        { "MVP", [](const Summary &s) { return (double)s.dmg; } },
        { L_(u8"Палач", u8"Executioner"), [](const Summary &s) { return (double)s.kills; } },
        { L_(u8"Пиковая мощь", u8"Peak power"), [](const Summary &s) { return (double)s.peak1; } },
        { L_(u8"Спасатель", u8"Lifesaver"), [](const Summary &s) { return (double)s.revives; } },
        { L_(u8"Медик", u8"Medic"), [](const Summary &s) { return (double)s.heal; } },
        { L_(u8"Стена", u8"Wall"), [](const Summary &s) { return (double)(s.taken + s.takenVeh); } },
        { L_(u8"Турист", u8"Tourist"), [](const Summary &s) { return (double)s.dist; } },
        { L_(u8"Предатель", u8"Traitor"), [](const Summary &s) { return (double)s.ffPlayers; } },
    };
    bool first = true;
    for (auto &a : aws) {
        const Row *b = nullptr; double bv = 0;
        for (auto &r : v.rows) if (!r.partial && a.f(r.s) > bv) { bv = a.f(r.s); b = &r; }
        if (!b) continue;
        if (!first) ImGui::SameLine(0, 18 * g_uiScale);
        ImGui::TextColored(GOLD, "%s", a.t); ImGui::SameLine(); ImGui::Text("%s", RowName(*b));
        first = false;
    }
    ImGui::Spacing();

    static const char *cols[] = { L_(u8"Игрок", u8"Player"), L_(u8"Урон", u8"Damage"), L_(u8"Пик DPS", u8"Peak DPS"), L_(u8"Убийства", u8"Kills"), L_(u8"Части", u8"Parts"), L_(u8"Лучший", u8"Best"), L_(u8"Получил", u8"Taken"),
                                  L_(u8"Смерти", u8"Deaths"), L_(u8"Поднял", u8"Rescues"), L_(u8"Лечение", u8"Healing"), L_(u8"По игрокам", u8"To players"), L_(u8"По NPC", u8"To NPCs"), L_(u8"Себе", u8"To self"), L_(u8"Пешком", u8"On foot"), L_(u8"Техника", u8"Vehicles") };
    const int nc = sizeof cols / sizeof cols[0];
    if (ImGui::BeginTable("t", nc, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
        for (int i = 0; i < nc; ++i) ImGui::TableSetupColumn(cols[i]);
        ImGui::TableHeadersRow();
        for (auto &r : v.rows) {
            const Summary &s = r.s;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(r.local ? GREEN : ImVec4(0.91f, 0.94f, 0.91f, 1), "%s", RowName(r));
            ImGui::SameLine(); ImGui::TextColored(DIM, "%s%s", s.cls, r.partial ? L_(u8" · неполн.", u8" · partial") : "");
            auto num = [](double x) { ImGui::TableNextColumn(); ImGui::Text("%s", NumSp(x).c_str()); };
            num(s.dmg); num(s.peak1); num(s.kills); num(s.parts); num(s.best); num(s.taken + s.takenVeh); num(s.deaths); num(s.revives); num(s.heal);
            ImGui::TableNextColumn(); if (s.ffPlayers > 0) ImGui::TextColored(RED, "%s", NumSp(s.ffPlayers).c_str()); else ImGui::Text("0");
            num(s.ffNpc); num(s.selfDmg);
            ImGui::TableNextColumn(); ImGui::Text(L_(u8"%s м", u8"%s m"), NumSp(s.dist).c_str());
            ImGui::TableNextColumn(); ImGui::Text(L_(u8"%s м", u8"%s m"), NumSp(s.distVeh).c_str());
        }
        ImGui::EndTable();
    }
    if (v.ended && !v.savedPath.empty()) ImGui::TextColored(DIM, L_(u8"Отчёт: Mods\\Plugins\\EDFStats\\latest.html", u8"Report: Mods\\Plugins\\EDFStats\\latest.html"));
    ImGui::End();
}

static void OverlayFrame(IDXGISwapChain *sc) {
    if (!g_imgui) { if (!OverlayInit(sc)) { g_ovDead = true; return; } g_imgui = true; }
    if (!g_rtv) MakeRtv(sc);
    if (!g_rtv) return;

    // hotkeys: only while the game window is in front
    bool fg = GetForegroundWindow() == g_hwnd;
    bool kl = fg && (GetAsyncKeyState(g_keyLive) & 0x8000), kb = fg && (GetAsyncKeyState(g_keyBoard) & 0x8000);
    if (kl && !g_kLive) g_showLive = !g_showLive;
    LiveView v; { EnterCriticalSection(&g_viewCs); v = g_view; LeaveCriticalSection(&g_viewCs); }
    if (kb && !g_kBoard) { if (g_autoBoard) { g_autoBoard = false; v.autoBoard = false; g_showBoard = false; } else g_showBoard = !g_showBoard; }
    g_kLive = kl; g_kBoard = kb;
    bool board = g_showBoard || g_autoBoard;
    if (!board && !g_showLive) return;

    ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
    if (g_showLive && !board && v.inMission) DrawLive(v);
    if (board) DrawBoard(v);
    ImGui::Render();
    ID3D11RenderTargetView *prev = nullptr; ID3D11DepthStencilView *prevDs = nullptr;
    g_ctx->OMGetRenderTargets(1, &prev, &prevDs);
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_ctx->OMSetRenderTargets(1, &prev, prevDs);
    if (prev) prev->Release(); if (prevDs) prevDs->Release();
}
static int OverlayFrameSafe(IDXGISwapChain *sc) {   // a crash in the overlay switches it off instead of taking the game down
    __try { OverlayFrame(sc); return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 1; }
}
static HRESULT __stdcall hPresent(IDXGISwapChain *sc, UINT sync, UINT flags) {
    if (g_overlayOn && !g_ovDead && OverlayFrameSafe(sc)) { g_ovDead = true; Log("overlay: exception, overlay switched off"); }
    return oPresent(sc, sync, flags);
}
static HRESULT __stdcall hResize(IDXGISwapChain *sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT fl) {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    return oResize(sc, n, w, h, fmt, fl);
}

// Present / ResizeBuffers addresses from a throw-away device on a hidden window
static void OverlayHook() {
    if (!g_overlayOn) { Log("overlay off (Overlay=0)"); return; }
    WNDCLASSEXA wc{ sizeof wc }; wc.lpfnWndProc = DefWindowProcA; wc.hInstance = GetModuleHandleA(nullptr); wc.lpszClassName = "EDFStatsProbe";
    RegisterClassExA(&wc);
    HWND w = CreateWindowExA(0, wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC d{}; d.BufferCount = 1; d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.BufferDesc.Width = 64; d.BufferDesc.Height = 64;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.OutputWindow = w; d.SampleDesc.Count = 1; d.Windowed = TRUE; d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain *sc = nullptr; ID3D11Device *dev = nullptr; ID3D11DeviceContext *ctx = nullptr;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &d, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr) || !sc) { Log("overlay: could not create a probe device (0x%08X), overlay off", (unsigned)hr); DestroyWindow(w); return; }
    void **vt = *(void ***)sc;
    void *present = vt[8], *resize = vt[13];
    sc->Release(); dev->Release(); ctx->Release(); DestroyWindow(w); UnregisterClassA(wc.lpszClassName, wc.hInstance);
    if (MH_CreateHook(present, (void *)hPresent, (void **)&oPresent) != MH_OK || MH_CreateHook(resize, (void *)hResize, (void **)&oResize) != MH_OK ||
        MH_EnableHook(present) != MH_OK || MH_EnableHook(resize) != MH_OK) { Log("overlay: could not hook Present, overlay off"); return; }
    Log("overlay hooks installed");
}
