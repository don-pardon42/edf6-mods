// MortarCam 0.1.1 (reconnaissance build) - EDF6 plugin for EDFModLoader.
// Fencer: a key switches the camera to a raised "artillery" view while a mortar is equipped, reusing the game's
// own mechanism of the Air Raider bomber view: the soldier's camera setting block soldier+0x230 / flag +0x224,
// which the character camera reads every frame. Purely local, nothing is sent over the network.
// This build also writes a detailed log (weapon slots, weapon states, predicted shell flight) for the next step.
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
#include <unordered_map>
#include "PluginAPI.h"
#include "MinHook.h"
#include "mortarcam_logic.h"

static const char *VERSION = "0.1.1";

// ---- signatures (EDF.dll build 2024-06-03) --------------------------------------------------------
// HeavyArmor (Fencer) per-frame update, vtable slot 5 (EDF.dll+5674B0)
static const char *SIG_HA_UPDATE = "48 89 5C 24 08 48 89 74 24 10 57 48 81 EC 80 00 00 00 48 8B D9 E8 ?? ?? ?? ?? 48 8D 8B 20 1F 00 00 E8";

// ---- layout (static analysis 2026-11-19) -----------------------------------------------------------
static const size_t SB_POS = 0x90;          // SoldierBase: world matrix row 3 = position
static const size_t SB_CAMBASE = 0x170;     // camera setting block from the soldier's data
static const size_t SB_CAMFLAG = 0x224;     // byte: 1 = camera uses the block at +0x230
static const size_t SB_CAMACT = 0x230;      // the block the camera uses when the flag is 1
static const size_t GO_DEAD = 0x2E8;
static const size_t SB_CTRL = 0x340;        // local controller, not 0 = this machine's player
static const size_t SB_RIDEFLAGS = 0x5D0;   // bit 4: Air Raider's update skips the bomber view then
static const size_t SB_VEHICLE = 0x1548;
// Weapons in hand (EDF.dll+5A42E0): soldier+0x1970 -> slot records (stride 0x150, count +0x1980); slot+0x40 points to
// an element of the soldier's weapon array (+0x1950, all carried weapons), which holds the weapon object.
// Fencer: 2 slots (two hands); switching the weapon set moves the slot pointers to the other array elements (0.1 log).
static const size_t SB_SLOTS = 0x1970, SB_NSLOTS = 0x1980, SLOT_SIZE = 0x150, SLOT_WEAPON = 0x40;
static const size_t W_FLAGS = 0x139;        // bytes +139..+13B: input/state flags (+13B: secondary fire active)
static const size_t W_142 = 0x142;          // byte, set to 1 for all Fencer weapons each frame
static const size_t W_NAME = 0x1B0;         // wchar_t*: weapon name
static const size_t W_MUZZLE = 0x1D0;       // object: rows +50/+60/+70, position +80
static const size_t W_BULLET = 0x240;       // wchar_t*: bullet class
static const size_t W_FIREVEC = 0x350;      // vec4 FireVector (local)
static const size_t W_SECTYPE = 0x690;      // SecondaryFire_Type
static const size_t W_SPEED = 0x894, W_ALIVE = 0x898, W_GRAVITY = 0x8E0;   // AmmoSpeed / AmmoAlive / AmmoGravityFactor
static const size_t W_STATE = 0x1610 + 0x18;   // weapon state machine: current state function

// ---- safe memory access (SEH, no syscalls) ---------------------------------------------------------
static bool SafeRead(const void *p, void *out, size_t n) {
    if ((uintptr_t)p < 0x10000 || (uintptr_t)p > 0x7FFFFFFFFFFFull) return false;
    __try { memcpy(out, p, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool SafeWrite(void *p, const void *in, size_t n) {
    if ((uintptr_t)p < 0x10000 || (uintptr_t)p > 0x7FFFFFFFFFFFull) return false;
    __try { memcpy(p, in, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template <class T> static T Rd(const void *p, T def = T()) { T v; return SafeRead(p, &v, sizeof v) ? v : def; }

// ---- logging (only on events; never per frame) ------------------------------------------------------
static FILE *g_log; static CRITICAL_SECTION g_cs; static uint8_t *g_base; static size_t g_size;
static void Log(const char *fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
    LeaveCriticalSection(&g_cs);
}
static std::string Hex(const uint8_t *p, size_t n) {
    std::string s; char b[4];
    for (size_t i = 0; i < n; ++i) { snprintf(b, sizeof b, "%02X", p[i]); s += b; if ((i & 3) == 3 && i + 1 < n) s += ' '; }
    return s;
}
static std::string Floats(const uint8_t *p, size_t n) {
    std::string s; char b[24];
    for (size_t i = 0; i + 4 <= n; i += 4) { float f; memcpy(&f, p + i, 4); snprintf(b, sizeof b, "%s%.3g", i ? " " : "", f); s += b; }
    return s;
}
static std::string Utf8(const wchar_t *w) {
    char b[256]; int k = WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, nullptr, nullptr);
    return k > 1 ? std::string(b, k - 1) : std::string();
}
static std::string WStrAt(const void *p) {           // up to 63 characters, "" if not readable / empty
    wchar_t w[64] = {};
    if (!SafeRead(p, w, sizeof w) && !SafeRead(p, w, 32)) return "";
    w[63] = 0;
    for (int i = 0; w[i]; ++i) if (w[i] < 0x20) return "";
    return Utf8(w);
}
static std::string WStrField(const uint8_t *obj, size_t off) {   // pointer to a string, else an inline string
    std::string s = WStrAt((const void *)Rd<uintptr_t>(obj + off));
    return s.empty() ? WStrAt(obj + off) : s;
}
static std::string ClassOf(const void *obj) {
    uintptr_t vt = Rd<uintptr_t>(obj);
    const uint8_t *col = vt ? Rd<const uint8_t *>((const void *)(vt - 8)) : nullptr;
    uint32_t td = col ? Rd<uint32_t>(col + 12) : 0;
    char buf[96] = {};
    if (!td || td > g_size || !SafeRead(g_base + td + 16, buf, 80)) return "?";
    std::string s(buf, strnlen(buf, 80));
    if (s.compare(0, 4, ".?AV") == 0) s = s.substr(4);
    size_t at = s.find("@@"); if (at != std::string::npos) s.resize(at);
    return s;
}
static uintptr_t Rva(uintptr_t p) { return (p >= (uintptr_t)g_base && p < (uintptr_t)g_base + g_size) ? p - (uintptr_t)g_base : p; }

// ---- settings -------------------------------------------------------------------------------------
static int g_key = 'B';
static bool g_hold = false, g_onlyMortar = true, g_probe = true;
static std::string g_match = "Mortar";
static mc::V3 g_look{ 0, 35, 0 }, g_camOff{ 0, 70, -140 };     // Air Raider's bomber view (0,50,0)*0.7 / (0,100,-200)*0.7
static float g_tilt = 0, g_up = 0.05f, g_down = 0.08f, g_gravity = 9.8f;
static int g_probeLimit = 400, g_statusMs = 1000;

// ---- per-player state -----------------------------------------------------------------------------
struct WInfo { uintptr_t vt = 0; std::string cls, name, bullet; bool weapon = false, mortar = false; };
static uint8_t *SlotWeapon(uint8_t *slots, uint32_t i) {   // *(*(slot + 0x40))
    uint8_t *ref = Rd<uint8_t *>(slots + i * SLOT_SIZE + SLOT_WEAPON);
    return ref ? Rd<uint8_t *>(ref) : nullptr;
}
static uint8_t *g_me;                        // local Fencer object
static std::unordered_map<uint8_t *, WInfo> g_wi;
static mc::Owner g_owner;
static float g_blend = 0;
static bool g_on = false, g_keyDown = false, g_baseLogged = false;
static int g_probeLines = 0;
static uint64_t g_lastStatus = 0;
static std::vector<uint64_t> g_slotKey;

static bool GameFocused() {
    DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}
static const WInfo &Info(uint8_t *w) {
    uintptr_t vt = Rd<uintptr_t>(w);
    auto it = g_wi.find(w);
    if (it != g_wi.end() && it->second.vt == vt) return it->second;
    WInfo wi; wi.vt = vt; wi.cls = ClassOf(w);
    wi.weapon = wi.cls.compare(0, 7, "Weapon_") == 0;
    if (!wi.weapon) { Log("slot object %p is %s, not a weapon; ignored", w, wi.cls.c_str()); return g_wi[w] = wi; }
    wi.name = WStrField(w, W_NAME); wi.bullet = WStrField(w, W_BULLET);
    wi.mortar = mc::NameMatches(wi.name, g_match);
    float fv[4] = {}; SafeRead(w + W_FIREVEC, fv, 16);
    Log("weapon %p %s \"%s\" bullet \"%s\" mortar=%d | speed %.3f alive %d gravity %.3f fireVec (%.2f %.2f %.2f) secondaryType %d",
        w, wi.cls.c_str(), wi.name.c_str(), wi.bullet.c_str(), wi.mortar, Rd<float>(w + W_SPEED), Rd<int>(w + W_ALIVE),
        Rd<float>(w + W_GRAVITY), fv[0], fv[1], fv[2], Rd<int>(w + W_SECTYPE));
    return g_wi[w] = wi;
}
static void Reset(uint8_t *me) {
    g_me = me; g_wi.clear(); g_owner = mc::Owner(); g_blend = 0; g_baseLogged = false; g_slotKey.clear(); g_probeLines = 0;
}

static bool Muzzle(uint8_t *w, mc::V3 &pos, mc::V3 &dir) {
    uint8_t *m = Rd<uint8_t *>(w + W_MUZZLE);
    float r[4][4], fv[4];
    if (!m || !SafeRead(m + 0x50, r, sizeof r) || !SafeRead(w + W_FIREVEC, fv, 16)) return false;
    dir = mc::V3{ r[0][0] * fv[0] + r[1][0] * fv[1] + r[2][0] * fv[2],
                  r[0][1] * fv[0] + r[1][1] * fv[1] + r[2][1] * fv[2],
                  r[0][2] * fv[0] + r[1][2] * fv[1] + r[2][2] * fv[2] };
    pos = mc::V3{ r[3][0], r[3][1], r[3][2] };
    return true;
}
static void LogShots(uint8_t *me, const std::vector<uint8_t *> &ws, const char *why) {
    float sp[3] = {}; SafeRead(me + SB_POS, sp, 12);
    Log("status (%s): you (%.1f %.1f %.1f) toggle %s blend %.2f camFlag %d owner %s", why, sp[0], sp[1], sp[2],
        g_on ? "ON" : "off", g_blend, Rd<uint8_t>(me + SB_CAMFLAG), g_owner.wrote ? "mod" : "game");
    for (uint8_t *w : ws) {
        const WInfo &wi = Info(w);
        if (!wi.mortar && Rd<float>(w + W_GRAVITY) <= 0) continue;
        mc::V3 p, d;
        if (!Muzzle(w, p, d)) { Log("  %s: muzzle not readable", wi.name.c_str()); continue; }
        mc::Shot s = mc::Predict(p, d, Rd<float>(w + W_SPEED), Rd<float>(w + W_GRAVITY), Rd<int>(w + W_ALIVE), sp[1], g_gravity);
        Log("  %s: muzzle (%.1f %.1f %.1f) dir (%.3f %.3f %.3f) -> %s impact (%.1f %.1f %.1f) dist %.1f m, %.2f s, apex %.1f",
            wi.name.c_str(), p.x, p.y, p.z, d.x, d.y, d.z, s.ok ? "predicted" : "no landing within AmmoAlive,",
            s.impact.x, s.impact.y, s.impact.z, s.dist, s.frames / 60.0f, s.apex);
    }
}

static void Frame(uint8_t *me) {
    if (Rd<uintptr_t>(me + SB_CTRL) == 0) return;            // not this machine's player
    if (me != g_me) { Log("local Fencer %p", me); Reset(me); }
    uint64_t now = GetTickCount64();

    bool focus = GameFocused();
    bool down = focus && (GetAsyncKeyState(g_key) & 0x8000) != 0;
    bool pressed = down && !g_keyDown; g_keyDown = down;
    bool prevOn = g_on;
    if (g_hold) g_on = down; else if (pressed) g_on = !g_on;

    // weapons in the slots (cheap reads only)
    static std::vector<uint8_t *> ws; ws.clear();     // reused: no allocations per frame
    static std::vector<uint64_t> key; key.clear();
    uint32_t n = Rd<uint32_t>(me + SB_NSLOTS); if (n > 8) n = 8;
    uint8_t *slots = Rd<uint8_t *>(me + SB_SLOTS);
    for (uint32_t i = 0; i < n && slots; ++i) {
        uint8_t *w = SlotWeapon(slots, i);
        if (!w || !Info(w).weapon) continue;
        ws.push_back(w);
        if (g_probe) {
            uint8_t f[4] = {}; SafeRead(w + W_FLAGS, f, 3); f[3] = Rd<uint8_t>(w + W_142);
            uint32_t fl; memcpy(&fl, f, 4);
            key.push_back((uint64_t)(uintptr_t)w); key.push_back(Rva(Rd<uintptr_t>(w + W_STATE)) ^ ((uint64_t)fl << 40));
        }
    }
    bool mortar = false;
    for (uint8_t *w : ws) if (Info(w).mortar) mortar = true;

    if (g_probe && key != g_slotKey && g_probeLines < g_probeLimit) {
        bool first = g_slotKey.empty();
        g_slotKey = key; ++g_probeLines;
        std::string line;
        for (uint32_t i = 0; i < n && slots; ++i) {
            uint8_t *w = SlotWeapon(slots, i);
            char b[200];
            if (!w) { snprintf(b, sizeof b, " [%u] empty", i); line += b; continue; }
            snprintf(b, sizeof b, " [%u] %s state %llX flags %02X%02X%02X/%02X", i, Info(w).name.c_str(),
                     (unsigned long long)Rva(Rd<uintptr_t>(w + W_STATE)), Rd<uint8_t>(w + W_FLAGS), Rd<uint8_t>(w + W_FLAGS + 1),
                     Rd<uint8_t>(w + W_FLAGS + 2), Rd<uint8_t>(w + W_142));
            line += b;
        }
        Log("slots:%s", line.c_str());
        if (first) {                                           // all carried weapons once per player object
            uint8_t **arr = Rd<uint8_t **>(me + 0x1950);
            uint64_t cnt = Rd<uint64_t>(me + 0x1958); if (cnt > 8) cnt = 8;
            for (uint64_t i = 0; i < cnt && arr; ++i) {
                uint8_t *w = Rd<uint8_t *>(arr + i);
                Log("  carried [%llu] %p %s", (unsigned long long)i, w, w ? Info(w).name.c_str() : "-");
            }
        }
    }

    bool dead = Rd<uint8_t>(me + GO_DEAD) != 0;
    // +5D0 bit 4 was also set during a Fencer dash (0.1 log), so only a vehicle blocks the camera
    bool riding = Rd<uintptr_t>(me + SB_VEHICLE) != 0;
    bool bit5d0 = (Rd<uint32_t>(me + SB_RIDEFLAGS) & 4) != 0;
    bool want = g_on && (mortar || !g_onlyMortar) && !dead && !riding;
    if (g_on != prevOn) {
        Log("toggle %s (mortar in hand: %s%s%s%s)", g_on ? "ON" : "OFF", mortar ? "yes" : "no", dead ? ", dead" : "",
            riding ? ", in vehicle" : "", bit5d0 ? ", flag +5D0&4" : "");
        LogShots(me, ws, "toggle");
    }

    uint8_t base[mc::BLOCK_SIZE], cur[mc::BLOCK_SIZE];
    if (!SafeRead(me + SB_CAMBASE, base, sizeof base) || !SafeRead(me + SB_CAMACT, cur, sizeof cur)) return;
    if (!g_baseLogged) {
        g_baseLogged = true;
        Log("camera setting block +170: %s", Floats(base, mc::BLOCK_SIZE).c_str());
        Log("  raw: %s", Hex(base, mc::BLOCK_SIZE).c_str());
        Log("camera flag +224 = %d", Rd<uint8_t>(me + SB_CAMFLAG));
    }
    bool flag = Rd<uint8_t>(me + SB_CAMFLAG) != 0;
    g_blend = mc::StepBlend(g_blend, want, g_up, g_down);
    switch (mc::Decide(flag, cur, g_owner, g_blend)) {
    case mc::Act::GameOwns:
        if (g_owner.wrote || g_blend > 0) Log("the game uses its own camera override now; MortarCam steps back");
        g_owner.wrote = false; g_blend = 0;
        break;
    case mc::Act::Write: {
        uint8_t out[mc::BLOCK_SIZE];
        mc::MakeBlock(base, out, g_blend, g_look, g_camOff, g_tilt);
        uint8_t one = 1;
        if (SafeWrite(me + SB_CAMACT, out, sizeof out) && SafeWrite(me + SB_CAMFLAG, &one, 1)) {
            if (!g_owner.wrote) Log("raised camera: on");
            memcpy(g_owner.last, out, sizeof out); g_owner.wrote = true;
        }
        break;
    }
    case mc::Act::Release: {
        uint8_t zero = 0; SafeWrite(me + SB_CAMFLAG, &zero, 1);
        g_owner.wrote = false;
        Log("raised camera: off (normal camera again)");
        break;
    }
    case mc::Act::Idle: break;
    }
    if (g_on && mortar && g_statusMs > 0 && now - g_lastStatus >= (uint64_t)g_statusMs) { g_lastStatus = now; LogShots(me, ws, "periodic"); }
}

// ---- hook ------------------------------------------------------------------------------------------
typedef void *(__fastcall *UpdFn)(void *, void *, void *, void *);
static UpdFn oUpd;
static void *__fastcall hUpd(void *self, void *a, void *b, void *c) {
    void *r = oUpd(self, a, b, c);
    Frame((uint8_t *)self);
    return r;
}

// ---- pattern scan + setup --------------------------------------------------------------------------
static bool ParseSig(const char *s, std::vector<int> &out) {
    while (*s) {
        while (*s == ' ') ++s;
        if (!*s) break;
        if (s[0] == '?') { out.push_back(-1); s += (s[1] == '?') ? 2 : 1; }
        else { out.push_back((int)strtoul(std::string(s, 2).c_str(), nullptr, 16)); s += 2; }
    }
    return !out.empty();
}
static uint8_t *Scan(const char *sig, bool hooked = false) {   // hooked: first 5 bytes already a jmp of another plugin
    std::vector<int> p; if (!ParseSig(sig, p)) return nullptr;
    if (hooked) { p[0] = 0xE9; for (int i = 1; i < 5; ++i) p[i] = -1; }
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
static int CharVk(char c) { SHORT vk = VkKeyScanA(c); return vk == -1 ? 0 : (vk & 0xFF); }

extern "C" __declspec(dllexport) bool EDFMLAPI EML6_Load(PluginInfo *info) {
    info->infoVersion = PluginInfo::MaxInfoVer;
    info->name = "MortarCam";
    info->version = PLUG_VER(0, 1, 1, 0);

    InitializeCriticalSection(&g_cs);
    char base[MAX_PATH]; HMODULE me = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&EML6_Load, &me);
    GetModuleFileNameA(me, base, MAX_PATH);
    char *dot = strrchr(base, '.'); if (dot) *dot = 0;
    std::string logPath = std::string(base) + ".log", ini = std::string(base) + ".ini";
    g_log = fopen(logPath.c_str(), "w");

    auto str = [&](const char *k, const char *def) {
        char v[512]; GetPrivateProfileStringA("MortarCam", k, def, v, sizeof v, ini.c_str()); return std::string(v);
    };
    auto flt = [&](const char *k, float def) { std::string s = str(k, ""); char *e; float f = strtof(s.c_str(), &e); return (s.empty() || *e) ? def : f; };
    auto num = [&](const char *k, int def) { return (int)GetPrivateProfileIntA("MortarCam", k, def, ini.c_str()); };
    g_key = mc::ParseKey(str("Key", "B").c_str(), 'B', CharVk);
    g_hold = num("HoldKey", 0) != 0;
    g_onlyMortar = num("OnlyWithMortar", 1) != 0;
    g_match = str("MortarNames", "Mortar");     // the ini is UTF-8; the bytes are compared with the UTF-8 weapon name
    mc::ParseV3(str("LookOffset", "0,35,0").c_str(), g_look);
    mc::ParseV3(str("CameraOffset", "0,70,-140").c_str(), g_camOff);
    g_tilt = flt("TiltDegrees", 0) * 3.14159265f / 180.0f;
    float sec = flt("BlendSeconds", 0.33f); g_up = sec > 0.01f ? 1.0f / (sec * 60.0f) : 1.0f; g_down = g_up * 1.5f;
    g_gravity = flt("Gravity", 9.8f);
    g_probe = num("ProbeLog", 1) != 0;
    g_probeLimit = num("ProbeLimit", 400);
    g_statusMs = num("StatusMs", 1000);
    Log("MortarCam %s loaded; key 0x%X (%s), onlyWithMortar %d, names \"%s\", look (%.1f %.1f %.1f) camera (%.1f %.1f %.1f) tilt %.1f deg, blend %.2f/frame, probe %d",
        VERSION, g_key, g_hold ? "hold" : "toggle", g_onlyMortar, g_match.c_str(), g_look.x, g_look.y, g_look.z,
        g_camOff.x, g_camOff.y, g_camOff.z, g_tilt * 180.0f / 3.14159265f, g_up, g_probe);

    HMODULE edf = GetModuleHandleW(L"EDF.dll");
    if (!edf) { Log("EDF.dll not found"); return false; }
    g_base = (uint8_t *)edf;
    g_size = ((IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew))->OptionalHeader.SizeOfImage;

    uint8_t *t = Scan(SIG_HA_UPDATE);
    if (!t && (t = Scan(SIG_HA_UPDATE, true))) Log("Fencer update already hooked by another plugin, chaining");
    if (!t) { Log("ERROR: signature 'Fencer update' not found (different game build?); not installing anything"); return false; }
    Log("Fencer update at EDF.dll+%llX", (unsigned long long)(t - g_base));
    MH_Initialize();
    if (MH_CreateHook(t, (void *)hUpd, (void **)&oUpd) != MH_OK || MH_EnableHook(t) != MH_OK) { Log("ERROR: hook failed"); return false; }
    Log("hook installed");
    return true;
}
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
