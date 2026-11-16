// SpectateCam 0.2 - EDF6 plugin for EDFModLoader (BlueAmulet).
// While your own character is dead, cycle the game's own character camera
// (CharacterGhostCamera) between other living soldiers; when you are alive
// again the camera returns to you. Purely local: nothing is sent over the network.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include "PluginAPI.h"
#include "MinHook.h"

// ---- signatures (EDF.dll build 2024-06-03, ?? = wildcard) ------------------------
static const char *SIG_CAM_CTOR   = "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC F0 00 00 00 48 89 54 24 40 48 8B F9 48 89 4D 87";
static const char *SIG_CAM_UPDATE = "48 89 5C 24 18 55 56 57 41 56 41 57 48 81 EC 90 00 00 00 0F 29 B4 24 80 00 00 00 44 0F 29 4C 24 70 48 8B 05 ?? ?? ?? ??";
static const char *SIG_CAM_DTOR   = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8B F2 48 8B F9 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 05 ?? ?? ?? ?? 48 89 41 08";
static const char *SIG_SB_CTOR    = "48 8B C4 48 89 58 18 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 88 FD FF FF 48 81 EC 40 03 00 00 0F 29 70 B8 0F 29 78 A8 44 0F 29 40 98 44 0F 29 48 88 44 0F 29 90 78 FF FF FF";
static const char *SIG_SB_DTOR    = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F9 48 8D 05 ?? ?? ?? ?? 48 89 01 48 8D 05 ?? ?? ?? ?? 48 89 81 20 01 00 00 48 8D 05 ?? ?? ?? ?? 48 89 81 40 19 00 00";

// ---- layout found by analysis + GhostCamDiag logs ----------------------------------
static const size_t CAM_TGT_PTR  = 0x350;   // CharacterGhostCamera: weak_ptr<target> {ptr, ctrl}
static const size_t CAM_TGT_CTRL = 0x358;
static const size_t SB_WEAK_PTR  = 0x28;    // SoldierBase: own weak_ptr (enable_shared_from_this)
static const size_t SB_WEAK_CTRL = 0x30;
static const size_t SB_HP        = 0x2F8;   // float, current HP (0 when dead)
static const size_t SB_CAMFLAG   = 0x224;   // byte: 1 for the local player, 0 for NPC; camera picks its offset block by it
static const size_t CAM_POS      = 0x3A0;   // camera: smoothed position vector (diagnostics)
static const size_t SB_MATRIX    = 0x60;    // SoldierBase: world matrix, row 3 (+0x90) = position

// ---- logging ------------------------------------------------------------------------
static FILE *g_log; static CRITICAL_SECTION g_logCs, g_regCs; static uint8_t *g_base;
static void Log(const char *fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_logCs);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
    LeaveCriticalSection(&g_logCs);
}
static bool Readable(const void *p, size_t n) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (const uint8_t *)p + n <= (const uint8_t *)mbi.BaseAddress + mbi.RegionSize;
}
static const char *ClassName(const void *obj) {
    static char buf[80]; strcpy(buf, "?");
    if (!Readable(obj, 8)) return buf;
    const uint8_t *vt = *(const uint8_t *const *)obj;
    if (!Readable(vt - 8, 8)) return buf;
    const uint8_t *col = *(const uint8_t *const *)(vt - 8);
    if (!Readable(col, 24)) return buf;
    const char *n = (const char *)(g_base + *(const uint32_t *)(col + 12) + 16);
    if (Readable(n, 4)) { snprintf(buf, sizeof buf, "%.60s", n); }
    return buf;
}

// ---- MSVC std::shared_ptr control block helpers -------------------------------------
// layout: {vtable, long uses (+8), long weaks (+0xC)}; vtable[1] = _Delete_this
static long Uses(uint8_t *ctrl) { return ctrl && Readable(ctrl + 8, 8) ? *(volatile long *)(ctrl + 8) : 0; }
static void IncWeak(uint8_t *ctrl) { if (ctrl) InterlockedIncrement((volatile long *)(ctrl + 0xC)); }
static void DecWeak(uint8_t *ctrl) {
    if (ctrl && InterlockedDecrement((volatile long *)(ctrl + 0xC)) == 0) {
        typedef void(__fastcall * DelFn)(void *);
        (*(DelFn *)(*(uint8_t **)ctrl + 8))(ctrl);
    }
}
// camera.target = weak_ptr{ptr, ctrl}   (copy-assign semantics)
static void SetCamTarget(uint8_t *cam, uint8_t *ptr, uint8_t *ctrl) {
    uint8_t *oldCtrl = *(uint8_t **)(cam + CAM_TGT_CTRL);
    IncWeak(ctrl);
    *(uint8_t **)(cam + CAM_TGT_PTR) = ptr;
    *(uint8_t **)(cam + CAM_TGT_CTRL) = ctrl;
    DecWeak(oldCtrl);
}

// ---- soldier registry ----------------------------------------------------------------
static std::vector<uint8_t *> g_sold;   // in creation order
typedef void *(__fastcall *SbCtorFn)(void *, void *); typedef void *(__fastcall *SbDtorFn)(void *);
static SbCtorFn oSbCtor; static SbDtorFn oSbDtor;
static void *__fastcall hSbCtor(void *self, void *init) {
    void *r = oSbCtor(self, init);
    EnterCriticalSection(&g_regCs); g_sold.push_back((uint8_t *)self); LeaveCriticalSection(&g_regCs);
    return r;
}
static void *__fastcall hSbDtor(void *self) {
    EnterCriticalSection(&g_regCs);
    for (size_t i = 0; i < g_sold.size(); ++i) if (g_sold[i] == self) { g_sold.erase(g_sold.begin() + i); break; }
    LeaveCriticalSection(&g_regCs);
    return oSbDtor(self);
}
static bool SoldierAlive(uint8_t *s) {
    if (!Readable(s, SB_HP + 4)) return false;
    uint8_t *ctrl = *(uint8_t **)(s + SB_WEAK_CTRL);
    if (*(uint8_t **)(s + SB_WEAK_PTR) != s || Uses(ctrl) <= 0) return false;
    return *(float *)(s + SB_HP) > 0.0f;
}

// ---- per-camera state ------------------------------------------------------------------
struct CamState { uint8_t *cam, *homePtr, *homeCtrl, *spect; };
static std::vector<CamState> g_cams;
static CamState *FindCam(void *cam) { for (auto &c : g_cams) if (c.cam == cam) return &c; return nullptr; }

static int g_keyNext = '=', g_keyPrev = '-', g_keySelf = '0', g_keyDebug = '9';
static bool g_down[4];
static bool g_debug = false;          // debug mode: switching allowed at any time
static bool g_npcPlayerCam = true;    // give NPC targets the player-style camera offset
static bool g_console = true;         // show the soldier list in a console window
static int  g_verifyFrames = 0;       // log a few frames after each switch
static CRITICAL_SECTION g_menuCs; static std::string g_menu;
static bool KeyPressed(int idx, int vk) {
    bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = d && !g_down[idx]; g_down[idx] = d; return edge;
}
static bool GameFocused() {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
static bool HomeDead(CamState &c) {
    if (!c.homePtr || Uses(c.homeCtrl) <= 0 || !Readable(c.homePtr + SB_HP, 4)) return false;
    return *(float *)(c.homePtr + SB_HP) <= 0.0f;
}
static void Spectate(CamState &c, uint8_t *s) {
    if (s == c.spect) return;
    if (!s) {
        SetCamTarget(c.cam, c.homePtr, c.homeCtrl);
        Log("camera back to own character %p", c.homePtr);
    } else {
        SetCamTarget(c.cam, s, *(uint8_t **)(s + SB_WEAK_CTRL));
        Log("spectating %p (%s, HP %.1f)", s, ClassName(s), *(float *)(s + SB_HP));
    }
    c.spect = s;
    g_verifyFrames = 3;
}
// pick the next/previous living soldier after the current one (nullptr = own character)
static uint8_t *Pick(CamState &c, int dir) {
    EnterCriticalSection(&g_regCs);
    std::vector<uint8_t *> list;
    for (auto s : g_sold) if (s != c.homePtr && SoldierAlive(s)) list.push_back(s);
    LeaveCriticalSection(&g_regCs);
    Log("  pick dir=%d: %zu living candidate(s)", dir, list.size());
    if (list.empty()) return nullptr;
    int cur = -1;
    for (size_t i = 0; i < list.size(); ++i) if (list[i] == c.spect) cur = (int)i;
    int n = (int)list.size() + 1;                 // slot n-1 == own character
    int pos = (cur < 0) ? n - 1 : cur;
    pos = ((pos + dir) % n + n) % n;
    return pos == n - 1 ? nullptr : list[pos];
}

// ---- camera hooks ------------------------------------------------------------------------
typedef void *(__fastcall *CamCtorFn)(void *, void *);
typedef void  (__fastcall *CamUpdateFn)(void *, float *);
typedef void *(__fastcall *CamDtorFn)(void *, unsigned);
static CamCtorFn oCamCtor; static CamUpdateFn oCamUpdate; static CamDtorFn oCamDtor;

static void *__fastcall hCamCtor(void *self, void *init) {
    void *r = oCamCtor(self, init);
    uint8_t *cam = (uint8_t *)self;
    CamState c{cam, *(uint8_t **)(cam + CAM_TGT_PTR), *(uint8_t **)(cam + CAM_TGT_CTRL), nullptr};
    IncWeak(c.homeCtrl);                           // our own weak ref to "home"
    g_cams.push_back(c);
    Log("camera %p created, own character %p (%s)", cam, c.homePtr, ClassName(c.homePtr));
    return r;
}
static void BuildMenu(CamState &c, bool dead) {
    std::string m;
    char line[256];
    snprintf(line, sizeof line, "SpectateCam 0.2   debug mode: %s   you: %s\n"
             "keys: next '%c'  prev '%c'  self '%c'  debug on/off '%c'\n\n",
             g_debug ? "ON" : "off", dead ? "DEAD" : "alive", g_keyNext, g_keyPrev, g_keySelf, g_keyDebug);
    m += line;
    EnterCriticalSection(&g_regCs);
    int i = 0;
    for (auto s : g_sold) {
        bool ok = Readable(s, SB_HP + 4);
        float hp = ok ? *(float *)(s + SB_HP) : 0.f;
        uint8_t *tgt = c.spect ? c.spect : c.homePtr;
        snprintf(line, sizeof line, "%s %2d  %-22s HP %7.1f  %s%s\n", s == tgt ? "=>" : "  ", ++i,
                 ClassName(s), hp, s == c.homePtr ? "[YOU] " : "", SoldierAlive(s) ? "" : "(dead)");
        m += line;
    }
    LeaveCriticalSection(&g_regCs);
    EnterCriticalSection(&g_menuCs); g_menu.swap(m); LeaveCriticalSection(&g_menuCs);
}
static void __fastcall hCamUpdate(void *self, float *dt) {
    static unsigned frame = 0;
    CamState *c = FindCam(self);
    uint8_t *flagObj = nullptr; uint8_t flagOld = 0;
    if (c) {
        bool dead = HomeDead(*c);
        bool focus = GameFocused();
        bool kN = KeyPressed(0, g_keyNext) && focus, kP = KeyPressed(1, g_keyPrev) && focus;
        bool kS = KeyPressed(2, g_keySelf) && focus, kD = KeyPressed(3, g_keyDebug) && focus;
        if (kD) { g_debug = !g_debug; Log("debug mode %s", g_debug ? "ON" : "OFF"); }
        if (kN || kP || kS)
            Log("key %s pressed: you are %s, debug %s, current target %p",
                kN ? "NEXT" : kP ? "PREV" : "SELF", dead ? "DEAD" : "alive", g_debug ? "on" : "off", c->spect);
        bool allowed = dead || g_debug;
        if (!allowed) {
            if (c->spect) Spectate(*c, nullptr);   // revived (and not in debug) -> back to you
        } else {
            if (kS) Spectate(*c, nullptr);
            else if (kN) Spectate(*c, Pick(*c, +1));
            else if (kP) Spectate(*c, Pick(*c, -1));
            else if (c->spect && !SoldierAlive(c->spect)) Spectate(*c, Pick(*c, +1));   // target died
        }
        // NPCs have the "player camera" flag at 0, so the camera would use a different offset block.
        // Set it to 1 only for the duration of this camera update, then restore.
        if (g_npcPlayerCam && c->spect && Readable(c->spect + SB_CAMFLAG, 1)) {
            flagObj = c->spect; flagOld = flagObj[SB_CAMFLAG]; flagObj[SB_CAMFLAG] = 1;
        }
        if (g_console && (frame++ % 15) == 0) BuildMenu(*c, dead);
    }
    oCamUpdate(self, dt);
    if (flagObj) flagObj[SB_CAMFLAG] = flagOld;
    if (c && g_verifyFrames > 0) {
        --g_verifyFrames;
        uint8_t *tp = *(uint8_t **)((uint8_t *)self + CAM_TGT_PTR);
        float *cp = (float *)((uint8_t *)self + CAM_POS);
        float *tm = (tp && Readable(tp + SB_MATRIX + 0x30, 12)) ? (float *)(tp + SB_MATRIX + 0x30) : nullptr;
        Log("  verify: camera target now %p, camPos (%.1f %.1f %.1f), targetPos %s(%.1f %.1f %.1f)",
            tp, cp[0], cp[1], cp[2], tm ? "" : "? ", tm ? tm[0] : 0.f, tm ? tm[1] : 0.f, tm ? tm[2] : 0.f);
    }
}
static DWORD WINAPI ConsoleThread(LPVOID) {
    AllocConsole();
    SetConsoleTitleA("SpectateCam - soldiers");
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    std::string last;
    for (;;) {
        Sleep(250);
        std::string m;
        EnterCriticalSection(&g_menuCs); m = g_menu; LeaveCriticalSection(&g_menuCs);
        if (m.empty() || m == last) continue;
        last = m;
        COORD home = {0, 0}; DWORD w; CONSOLE_SCREEN_BUFFER_INFO bi;
        GetConsoleScreenBufferInfo(out, &bi);
        FillConsoleOutputCharacterA(out, ' ', bi.dwSize.X * bi.dwSize.Y, home, &w);
        SetConsoleCursorPosition(out, home);
        WriteConsoleA(out, m.c_str(), (DWORD)m.size(), &w, nullptr);
    }
    return 0;
}
static void *__fastcall hCamDtor(void *self, unsigned flags) {
    for (size_t i = 0; i < g_cams.size(); ++i)
        if (g_cams[i].cam == self) {
            if (g_cams[i].spect) Spectate(g_cams[i], nullptr);
            DecWeak(g_cams[i].homeCtrl);
            g_cams.erase(g_cams.begin() + i);
            break;
        }
    return oCamDtor(self, flags);
}

// ---- pattern scan + setup --------------------------------------------------------------
static bool ParseSig(const char *s, std::vector<int> &out) {
    while (*s) {
        while (*s == ' ') ++s;
        if (!*s) break;
        if (s[0] == '?') { out.push_back(-1); s += (s[1] == '?') ? 2 : 1; }
        else { out.push_back((int)strtoul(std::string(s, 2).c_str(), nullptr, 16)); s += 2; }
    }
    return !out.empty();
}
static uint8_t *Scan(const char *sig) {
    std::vector<int> p; if (!ParseSig(sig, p)) return nullptr;
    auto nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    uint8_t *found = nullptr; int count = 0;
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (memcmp(sec[i].Name, ".text", 5) != 0) continue;
        uint8_t *b = g_base + sec[i].VirtualAddress; size_t n = sec[i].Misc.VirtualSize;
        for (size_t k = 0; k + p.size() <= n; ++k) {
            size_t j = 0;
            while (j < p.size() && (p[j] < 0 || b[k + j] == (uint8_t)p[j])) ++j;
            if (j == p.size()) { found = b + k; ++count; }
        }
    }
    return count == 1 ? found : nullptr;
}
static uint8_t *Find(const char *name, const char *sig) {
    uint8_t *t = Scan(sig);
    if (!t) Log("ERROR: signature %s not found (different game build?)", name);
    return t;
}

extern "C" __declspec(dllexport) bool EDFMLAPI EML6_Load(PluginInfo *info) {
    info->infoVersion = PluginInfo::MaxInfoVer;
    info->name = "SpectateCam";
    info->version = PLUG_VER(0, 2, 0, 0);

    InitializeCriticalSection(&g_logCs); InitializeCriticalSection(&g_regCs);
    char base[MAX_PATH]; HMODULE me = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&EML6_Load, &me);
    GetModuleFileNameA(me, base, MAX_PATH);
    char *dot = strrchr(base, '.'); if (dot) *dot = 0;
    std::string logPath = std::string(base) + ".log", iniPath = std::string(base) + ".ini";
    g_log = fopen(logPath.c_str(), "w");
    // value may be a single character ("=", "-", "0") or a virtual-key code (0xBB, 187)
    auto readKey = [&](const char *k, int def) {
        char v[32]; GetPrivateProfileStringA("SpectateCam", k, "", v, sizeof v, iniPath.c_str());
        if (!v[0]) return def;
        if (v[1] == 0) { SHORT vk = VkKeyScanA(v[0]); return (vk == -1) ? def : (int)(vk & 0xFF); }
        long x = strtol(v, nullptr, 0);
        return (x > 0 && x < 256) ? (int)x : def;
    };
    g_keyNext = readKey("NextKey", '=');
    g_keyPrev = readKey("PrevKey", '-');
    g_keySelf = readKey("SelfKey", '0');
    g_keyDebug = readKey("DebugKey", '9');
    g_debug = GetPrivateProfileIntA("SpectateCam", "DebugAtStart", 0, iniPath.c_str()) != 0;
    g_npcPlayerCam = GetPrivateProfileIntA("SpectateCam", "NpcPlayerCamera", 1, iniPath.c_str()) != 0;
    g_console = GetPrivateProfileIntA("SpectateCam", "Console", 1, iniPath.c_str()) != 0;
    InitializeCriticalSection(&g_menuCs);
    Log("SpectateCam 0.2 loaded; keys next=0x%X prev=0x%X self=0x%X debug=0x%X; debugAtStart=%d npcPlayerCam=%d console=%d",
        g_keyNext, g_keyPrev, g_keySelf, g_keyDebug, g_debug, g_npcPlayerCam, g_console);

    HMODULE edf = GetModuleHandleW(L"EDF.dll");
    if (!edf) { Log("EDF.dll not found"); return false; }
    g_base = (uint8_t *)edf;

    // find everything first; install nothing unless all five are found
    uint8_t *t[5] = { Find("SoldierBase ctor", SIG_SB_CTOR), Find("SoldierBase dtor", SIG_SB_DTOR),
                      Find("Camera ctor", SIG_CAM_CTOR), Find("Camera update", SIG_CAM_UPDATE), Find("Camera dtor", SIG_CAM_DTOR) };
    for (auto p : t) if (!p) { Log("not installing any hooks"); return false; }
    void *det[5] = { (void *)hSbCtor, (void *)hSbDtor, (void *)hCamCtor, (void *)hCamUpdate, (void *)hCamDtor };
    void **orig[5] = { (void **)&oSbCtor, (void **)&oSbDtor, (void **)&oCamCtor, (void **)&oCamUpdate, (void **)&oCamDtor };
    MH_Initialize();
    for (int i = 0; i < 5; ++i)
        if (MH_CreateHook(t[i], det[i], orig[i]) != MH_OK) { Log("ERROR: MH_CreateHook %d", i); return false; }
    for (int i = 0; i < 5; ++i) MH_EnableHook(t[i]);
    Log("all hooks installed");
    if (g_console) CreateThread(nullptr, 0, ConsoleThread, nullptr, 0, nullptr);
    return true;
}
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
