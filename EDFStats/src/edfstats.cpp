// EDFStats 0.9 - EDF6 player statistics plugin for EDFModLoader.
// Hooks GameObjectBase::ApplyDamage (EDF.dll+547960, build 2024-06-03), the single function
// every hit / heal on every game object goes through, and counts for this machine's player:
// damage, peak DPS, kills, destroyed parts, friendly fire, damage taken, deaths, damage per weapon,
// vehicle damage, kills per enemy type, distance travelled.
// Every player runs the plugin; each one sends its own numbers to the others through the game's
// own Epic Online Services P2P connection (separate socket "EDFStats", no IPs or ports needed),
// plus optionally over UDP to the peers from EDFStats.ini / SquadSync.ini. Names come from Steam.
// Results are written on F9 and automatically when the next mission starts:
//   EDFStats.log, EDFStats\missions\<date>.json, EDFStats\missions\<date>.html, EDFStats\latest.html
//
// Verified in game (logs of 2026-11-14, two players):
//   info+0x10  attacker object*         info+0x24 int attacker team    info+0x50 float damage (<0 heal)
//   info+0x60  u16 flags: 0x20 fired by a player, 0x03 explosive/area
//   obj+0x2E8  byte dead   +0x2F4 max HP   +0x2F8 HP   +0x314 team (becomes -1 when the object dies)
//   SoldierBase +0x090 position (world matrix row 3), +0x340 local controller (!=0 only for this
//               machine's player), +0x1548 vehicle the soldier is riding, +0x1ED0 set for players
//   Only the machine that owns the shooter reduces HP for its hits -> each client counts its own player.
//   Friendliness: the game's team relation table, ApplyDamage+0x19B: mov rcx,[rip+X].
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <unordered_map>
#include <algorithm>
#include <intrin.h>
#include "PluginAPI.h"
#include "MinHook.h"
#pragma comment(lib, "ws2_32.lib")

static const char *VERSION = "0.12";
// ---- language: Russian for ru/uk/be/kk Windows, English otherwise; EDFStats.ini Language=auto|ru|en overrides
static bool g_en = false;
static inline const char *L_(const char *ru, const char *en) { return g_en ? en : ru; }
static void PickLanguage(const std::string &setting) {
    if (setting == "ru") { g_en = false; return; }
    if (setting == "en") { g_en = true; return; }
    WORD p = PRIMARYLANGID(GetUserDefaultUILanguage());
    g_en = !(p == LANG_RUSSIAN || p == LANG_UKRAINIAN || p == LANG_BELARUSIAN || p == LANG_KAZAK);
}
static const char *SIG_APPLY_DAMAGE =
    "48 8B C4 48 89 58 18 55 56 57 41 54 41 55 41 56 41 57 48 8D A8 38 FF FF FF 48 81 EC 90 01 00 00 "
    "0F 29 70 B8 0F 29 78 A8 44 0F 29 40 98 44 0F 29 48 88 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 40 4C 8B EA 48";
static const size_t FN_TEAMTABLE = 0x19B;
// ui::HUiMissionResult constructor (EDF.dll+8D28C0): the game opens the mission result / rewards screen
static const char *SIG_RESULT_CTOR = "48 89 5C 24 18 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 60 FB FF FF";
// static SoldierBase *SoldierBase::CreateOnlinePlayerObject(unsigned slot, const Matrix &where, EquipWeaponType)
// (EDF.dll+590E60): a player character is spawned in an online game (mission start; the game also replaces
// a player's object now and then, stats are moved to the new object)
static const char *SIG_CREATE_PLAYER = "48 89 5C 24 08 48 89 74 24 10 55 57 41 54 41 56 41 57 48 8D 6C 24 C9 48 81 EC F0 00 00 00";
// its deleting destructor (EDF.dll+8D66B0): the result screen closes
static const char *SIG_RESULT_DTOR = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B D9 48 89 01 8B EA 48 8B 89 D0 07 00 00";

static const size_t GO_DEAD = 0x2E8, GO_HP = 0x2F8, GO_TEAM = 0x314;
static const size_t SB_POS = 0x90, SB_SLOT = 0x338, SB_CTRL = 0x340, SB_VEHICLE = 0x1548, SB_NETPLAYER = 0x1ED0;
static const size_t DI_ATTACKER = 0x10, DI_TEAM = 0x24, DI_DAMAGE = 0x50, DI_FLAGS = 0x60;

// ---- safe memory access (SEH, no syscalls) -------------------------------------------------
static bool SafeRead(const void *p, void *out, size_t n) {
    if ((uintptr_t)p < 0x10000 || (uintptr_t)p > 0x7FFFFFFFFFFFull) return false;
    __try { memcpy(out, p, n); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
template <class T> static T Rd(const void *p, T def = T()) { T v; return SafeRead(p, &v, sizeof v) ? v : def; }

// ---- logging ------------------------------------------------------------------------------
static FILE *g_log; static CRITICAL_SECTION g_cs; static uint8_t *g_base; static std::string g_dir;
static int g_logHits = 0, g_dumpKey = VK_F9, g_resetKey = VK_F10;
static void Logv(bool flush, const char *fmt, va_list a) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    vfprintf(g_log, fmt, a); fputc('\n', g_log);
    if (flush) fflush(g_log);
}
static void Log(const char *fmt, ...) { va_list a; va_start(a, fmt); Logv(true, fmt, a); va_end(a); }
static void LogHit(const char *fmt, ...) { va_list a; va_start(a, fmt); Logv(false, fmt, a); va_end(a); }

// ---- class info, cached per vtable ---------------------------------------------------------
enum Kind : uint8_t { K_OTHER, K_SOLDIER, K_VEHICLE, K_WEAPON, K_BULLET };
struct ClsInfo { std::string name; Kind kind = K_OTHER; };
static std::unordered_map<uintptr_t, ClsInfo> g_cls;            // guarded by g_cs
static const ClsInfo &Cls(const uint8_t *obj) {
    static const ClsInfo none;
    uintptr_t vt = Rd<uintptr_t>(obj);
    if (!vt) return none;
    auto it = g_cls.find(vt);
    if (it != g_cls.end()) return it->second;
    ClsInfo ci;
    const uint8_t *col = Rd<const uint8_t *>((const void *)(vt - 8));
    uint32_t td = col ? Rd<uint32_t>(col + 12) : 0;
    char buf[128] = {};
    if (td && (SafeRead(g_base + td + 16, buf, 127) || SafeRead(g_base + td + 16, buf, 40))) {
        std::string s(buf, strnlen(buf, 127));
        if (s.compare(0, 4, ".?AV") == 0) s = s.substr(4);
        size_t at = s.find("@@"); if (at != std::string::npos) s.resize(at);
        ci.name = s;
    }
    const std::string &n = ci.name;
    if (n == "AssultSoldier" || n == "PaleWing" || n == "HeavyArmor" || n == "Engineer") ci.kind = K_SOLDIER;
    else if (n.compare(0, 7, "Vehicle") == 0) ci.kind = K_VEHICLE;
    else if (n.compare(0, 7, "Weapon_") == 0) ci.kind = K_WEAPON;                       // Weapon_BasicShoot, Weapon_VehicleShoot ...
    else if (n.find("Bullet") != std::string::npos && n.find('@') == std::string::npos && n.find("System") == std::string::npos &&
             n.find("Listener") == std::string::npos && n.find("Collision") == std::string::npos && n.find("Model") == std::string::npos &&
             n.find("Table") == std::string::npos && n.find("Manager") == std::string::npos && n.find("Mark") == std::string::npos)
        ci.kind = K_BULLET;                                                              // SolidBullet01, MissileBullet01 ...
    return g_cls.emplace(vt, ci).first->second;
}
static const char *ClassName(const std::string &c) {
    if (c == "AssultSoldier") return "Ranger";
    if (c == "PaleWing") return "Wing Diver";
    if (c == "HeavyArmor") return "Fencer";
    if (c == "Engineer") return "Air Raider";
    return c.c_str();
}

// destroyable pieces of a bigger enemy (Berserker equipment, mothership batteries...) - not kills
static bool IsPart(const std::string &c) {
    for (const char *k : { "Equipment", "Parts", "Battery", "Barrier", "Hatch", "Cannon", "_Arm", "Generator", "Anchor", "EggSac" })
        if (c.find(k) != std::string::npos) return true;
    return false;
}
static std::string EnemyName(const std::string &c) {     // class name -> UTF-8 name in the interface language
    static const struct { const char *cls, *ru, *en; } names[] = {
        { "GiantAnt", u8"Муравей", "Ant" }, { "GiantSpider", u8"Паук", "Spider" }, { "GiantBee", u8"Оса", "Wasp" },
        { "GiantAntEgg", u8"Яйцо муравья", "Ant egg" },
        { "EDF6_Berserker_TypeA", u8"Берсеркер", "Berserker" }, { "EDF6_Berserker_Middle", u8"Берсеркер (средний)", "Berserker (medium)" },
        { "EDF6_Berserker_Large", u8"Берсеркер (большой)", "Berserker (large)" }, { "EDF6_Berserker_TypeBommer", u8"Берсеркер-бомбардир", "Berserker bomber" },
        { "EDF6_Berserker_TypeGrapple", u8"Берсеркер с тросом", "Grappling Berserker" },
        { "EDF6_Berserker_Equipment_RocketPunch", u8"Ракетный кулак берсеркера", "Berserker rocket fist" },
        { "EDF6_Berserker_Equipment_Bomb", u8"Бомба берсеркера", "Berserker bomb" }, { "EDF6_Berserker_Equipment_GrappleWire", u8"Трос берсеркера", "Berserker grapple wire" },
        { "DragonSmall", u8"Дракон", "Dragon" }, { "HumanoidBase", u8"Колонист", "Colonist" }, { "Humanoid_BigGreyBoss", u8"Большой серый", "Big grey" },
        { "AntHill", u8"Муравейник", "Ant hill" }, { "BigNest", u8"Гнездо", "Nest" },
    };
    for (auto &n : names) if (c == n.cls) return L_(n.ru, n.en);
    return c.compare(0, 5, "EDF6_") == 0 ? c.substr(5) : c;
}

// ---- team relation table --------------------------------------------------------------------
static uint8_t **g_teamTable;
static int Friendly(int a, int b) {       // 1 friendly, 0 hostile, -1 unknown
    if (a < 0 || b < 0 || a > 64 || b > 64 || !g_teamTable) return -1;
    uint8_t *t = Rd<uint8_t *>(g_teamTable); if (!t) return -1;
    uint8_t *rows = Rd<uint8_t *>(t + 0x38); if (!rows) return -1;
    int32_t *row = Rd<int32_t *>(rows + a * 0x38 + 0x18); if (!row) return -1;
    int32_t v = Rd<int32_t>(row + b, -1);
    return v < 0 ? -1 : v == 1;
}

// ---- per player statistics --------------------------------------------------------------------
struct DpsMeter {                    // 100 ms buckets over the last 5 s
    double b[50] = {}; int64_t last = -1; double peak1 = 0, peak5 = 0;
    void Add(uint64_t ms, double v) {
        int64_t k = (int64_t)(ms / 100);
        if (last < 0) last = k;
        if (k - last >= 50) { memset(b, 0, sizeof b); last = k; }
        while (last < k) { ++last; b[last % 50] = 0; }
        b[k % 50] += v;
        double s1 = 0, s5 = 0;
        for (int i = 0; i < 50; ++i) { double x = b[(k - i + 500) % 50]; s5 += x; if (i < 10) s1 += x; }
        peak1 = std::max(peak1, s1); peak5 = std::max(peak5, s5 / 5.0);
    }
    double Window(uint64_t ms, int n) const {     // damage in the last n buckets up to now
        if (last < 0) return 0;
        int64_t k = (int64_t)(ms / 100); double sum = 0;
        for (int i = 0; i < n; ++i) { int64_t j = k - i; if (j <= last && j > last - 50) sum += b[j % 50]; }
        return sum;
    }
};
struct WStats {                      // one weapon of one player
    double dmg = 0, best = 0; unsigned hits = 0, kills = 0; DpsMeter dps;
    int64_t lastSec = -1; unsigned activeSecs = 0;          // seconds in which this weapon hit something
    bool veh = false;
    void Add(uint64_t ms, double v, bool kill) {
        dmg += v; ++hits; best = std::max(best, v); dps.Add(ms, v); if (kill) ++kills;
        int64_t sec = (int64_t)(ms / 1000); if (sec != lastSec) { lastSec = sec; ++activeSecs; }
    }
};
struct PStats {
    std::string cls;
    double dmg = 0, dmgVeh = 0, best = 0, dist = 0, distVeh = 0;
    double ffNpc = 0, ffPlayersSeen = 0, selfDmg = 0;      // dealt to allied NPCs / to other players / to yourself
    double taken = 0, takenVeh = 0, takenFF = 0;            // received from enemies (on foot / in a vehicle) / from allies
    unsigned hits = 0, kills = 0, killsVeh = 0, parts = 0, ffKills = 0, deaths = 0;   // ffKills: allied NPCs killed
    std::map<std::string, unsigned> killsBy;
    std::map<std::string, double> dmgBy;
    std::map<std::string, double> ffFrom;                   // friendly damage received from other players, by their class
    DpsMeter dps;
    std::map<uint8_t *, WStats> weapons;                    // key: weapon object (nullptr = not recognised); this PC only
};
static std::map<uint8_t *, PStats> g_ps;            // key: soldier object of a player
static uint8_t *g_localSol;
static uint64_t g_missionStartMs; static SYSTEMTIME g_missionStart;
static uint32_t g_missionId = 1;
static unsigned g_hitLines;

static PStats &P(uint8_t *sol) {
    PStats &p = g_ps[sol];
    if (p.cls.empty()) p.cls = Cls(sol).name;
    return p;
}
static bool IsPlayer(uint8_t *sol) { return Rd<void *>(sol + SB_NETPLAYER) != nullptr; }
static bool IsLocal(uint8_t *sol)  { return Rd<void *>(sol + SB_CTRL) != nullptr; }
static uint8_t *Driver(uint8_t *v) {             // player soldier that drives vehicle v
    if (g_localSol && Rd<uint8_t *>(g_localSol + SB_VEHICLE) == v) return g_localSol;
    for (auto &kv : g_ps) if (Rd<uint8_t *>(kv.first + SB_VEHICLE) == v) return kv.first;
    return nullptr;
}

// ---- weapons: which weapon a hit came from -------------------------------------------------------
// A weapon object (Weapon_* class) fires bullet objects (*Bullet* classes). While a hit is applied, the
// bullet and/or its weapon are still referenced from the callers' stack frames, so the stack above the hook
// is scanned for them; a bullet found there is searched for the pointer to its weapon. Every class keeps
// its own weapon objects, so the Fencer's four weapons (two of them firing at the same time) stay apart.
// WeaponProbe=1 (EDFStats.ini) logs what was found, to check and improve this in the game.
static uintptr_t g_vtLo, g_vtHi;              // EDF.dll .rdata: where vtables live
static bool g_wprobe = true; static unsigned g_wprobeHits;
static bool GameObj(uintptr_t p, Kind *k = nullptr) {
    if (p < 0x10000 || p > 0x7FFFFFFFFFFFull || (p & 7)) return false;
    uintptr_t vt = Rd<uintptr_t>((const void *)p);
    if (vt < g_vtLo || vt >= g_vtHi) return false;
    if (k) *k = Cls((const uint8_t *)p).kind;
    return true;
}
static bool WcharOk(wchar_t c) { return (c >= 0x20 && c < 0x7F) || (c >= 0x400 && c < 0x460) || c == 0x2116 || c == 0x2013 || c == 0x2014 || c == 0xB7 || c == 0xAB || c == 0xBB; }
static std::string Utf8(const wchar_t *w, int n) {
    char b[256]; int k = WideCharToMultiByte(CP_UTF8, 0, w, n, b, sizeof b, nullptr, nullptr);
    return std::string(b, k > 0 ? k : 0);
}
// a UTF-16 string of 3..60 good characters at p (zero terminated), "" otherwise
static std::string ReadWStr(const void *p) {
    wchar_t w[64] = {};
    if (!SafeRead(p, w, sizeof w) && !SafeRead(p, w, 32)) return "";
    int n = 0; while (n < 63 && w[n]) { if (!WcharOk(w[n])) return ""; ++n; }
    if (n < 3 || n >= 61) return "";
    return Utf8(w, n);
}
static bool LooksLikeName(const std::string &u) {     // weapon names: no paths, ids or file names
    if (u.size() < 3) return false;
    for (char c : u) if (c == '/' || c == '\\' || c == '_' || c == ':' || c == '.' || c == '%' || c == '=') return false;
    bool letter = false; for (unsigned char c : u) if (isalpha(c) || c >= 0xD0) letter = true;
    return letter;
}
struct WInfo { std::string name; };
static std::unordered_map<uint8_t *, WInfo> g_weapons;      // weapon object -> its name (this mission)
static void LogWeaponObject(uint8_t *w, uint8_t *owner) {   // WeaponProbe: strings, objects and the owner in a weapon object
    std::set<std::string> seen; int lines = 0; std::string own;
    for (size_t o = 0; o < 0x1800; o += 8) if (owner && Rd<uint8_t *>(w + o) == owner) { char b[16]; snprintf(b, sizeof b, " +%zX", o); own += b; }
    Log("    probe weapon %p owner (shooter) at:%s", w, own.empty() ? " none" : own.c_str());
    for (size_t o = 0; o < 0x1800 && lines < 40; o += 8) {
        std::string in = ReadWStr(w + o);
        if (!in.empty() && seen.insert(in).second) { Log("    probe weapon %p +%03zX inline \"%s\"", w, o, in.c_str()); ++lines; }
        uintptr_t q = Rd<uintptr_t>(w + o);
        if (q < 0x10000 || q > 0x7FFFFFFFFFFFull) continue;
        std::string ps = ReadWStr((const void *)q);
        if (!ps.empty() && seen.insert(ps).second) { Log("    probe weapon %p +%03zX -> \"%s\"", w, o, ps.c_str()); ++lines; }
        Kind k; if (GameObj(q, &k) && k != K_WEAPON) { std::string cn = Cls((uint8_t *)q).name; if (seen.insert(cn).second) { Log("    probe weapon %p +%03zX -> object %s", w, o, cn.c_str()); ++lines; } }
    }
}
static int g_weaponNo;
static uint8_t *g_hitOwner;               // the shooter of the hit being matched (WeaponProbe)
static const WInfo &Weapon(uint8_t *w) {
    auto it = g_weapons.find(w);
    if (it != g_weapons.end()) return it->second;
    WInfo wi;
    { std::string n0 = ReadWStr((const void *)Rd<uintptr_t>(w + 0x1B0)); if (LooksLikeName(n0)) wi.name = n0; }   // the weapon's name (2026-11-18 log)
    for (size_t o = 0; o < 0x1800 && wi.name.empty(); o += 8) {          // a name the HUD would show
        uintptr_t q = Rd<uintptr_t>(w + o);
        std::string c = (q >= 0x10000 && q <= 0x7FFFFFFFFFFFull) ? ReadWStr((const void *)q) : "";
        if (c.empty()) c = ReadWStr(w + o);
        if (LooksLikeName(c)) wi.name = c;
    }
    if (wi.name.empty()) {                       // not found: class name and a number
        std::string cn = Cls(w).name; if (cn.compare(0, 7, "Weapon_") == 0) cn = cn.substr(7);
        char b[96]; snprintf(b, sizeof b, "%s #%d", cn.c_str(), ++g_weaponNo); wi.name = b;
    }
    if (g_wprobe) { Log("probe: new weapon %p %s -> name \"%s\"", w, Cls(w).name.c_str(), wi.name.c_str()); LogWeaponObject(w, g_hitOwner); }
    return g_weapons.emplace(w, wi).first->second;
}
static std::string WeaponName(uint8_t *w) { return w ? Weapon(w).name : L_(u8"Не определено", u8"Not recognised"); }
// stack: address of the hook's return address; the callers' frames are above it
// The weapons of a shooter (soldier or vehicle) and the values that only one of them holds
// (its bullet factory, parameter blocks ...). A bullet, a bullet record of BulletMtSystem or the damage
// info usually refers to one of those, which tells the weapon even when nothing points to it directly
// (2026-11-18 test: bullets do not point to their weapon; vehicle weapons sit on the stack).
static bool HeapPtr(uintptr_t v) { return v >= 0x10000 && v <= 0x7FFFFFFFFFFFull && !(v & 7); }
struct WIdx { std::vector<uint8_t *> list; std::unordered_map<uintptr_t, uint8_t *> uniq; uint64_t builtMs = 0; };
static std::unordered_map<uint8_t *, WIdx> g_widx;          // shooter -> its weapons (this mission)
static const size_t W_SPAN = 0x1800;                        // bytes of a weapon object that are looked at
static void AddWeapon(std::vector<uint8_t *> &out, uintptr_t v) {
    Kind k; if (!GameObj(v, &k) || k != K_WEAPON || std::find(out.begin(), out.end(), (uint8_t *)v) != out.end()) return;
    if (Cls((uint8_t *)v).name == "Weapon_Accessory") return;      // Fencer's add-on equipment never fires
    out.push_back((uint8_t *)v);
}
static WIdx &WeaponsOf(uint8_t *owner, uint64_t now, bool force = false) {
    WIdx &x = g_widx[owner];
    if (x.builtMs && now - x.builtMs < (force ? 500u : 3000u)) return x;
    x.builtMs = now;
    std::vector<uint8_t *> found;
    for (size_t o = 0; o < 0x2400; o += 8) {                // the owner object and arrays it points to
        uintptr_t v = Rd<uintptr_t>(owner + o);
        if (!HeapPtr(v)) continue;
        Kind k; if (GameObj(v, &k)) { if (k == K_WEAPON) AddWeapon(found, v); continue; }
        uintptr_t arr[16]; if (SafeRead((const void *)v, arr, sizeof arr)) for (uintptr_t a : arr) AddWeapon(found, a);
    }
    for (uint8_t *w : x.list) if (std::find(found.begin(), found.end(), w) == found.end() && GameObj((uintptr_t)w)) found.push_back(w);   // seen on the stack before
    bool changed = found != x.list;
    x.list = found;
    std::unordered_map<uintptr_t, int> cnt; std::unordered_map<uintptr_t, uint8_t *> who;
    for (uint8_t *w : x.list) {
        std::set<uintptr_t> mine;
        for (size_t o = 0; o < W_SPAN; o += 8) {
            uintptr_t v = Rd<uintptr_t>(w + o);
            if (!HeapPtr(v) || (uint8_t *)v == owner || (v >= (uintptr_t)w && v < (uintptr_t)w + W_SPAN)) continue;
            if (!GameObj(v)) continue;                // only game objects (bullet factory, model ...): plain values match by chance
            if (mine.insert(v).second) { ++cnt[v]; who[v] = w; }
        }
    }
    x.uniq.clear();
    for (auto &c : cnt) if (c.second == 1) x.uniq[c.first] = who[c.first];
    if (changed && g_wprobe) {
        Log("probe: %s %p has %zu weapon(s), %zu values unique to one weapon", Cls(owner).name.c_str(), owner, x.list.size(), x.uniq.size());
        for (uint8_t *w : x.list) Log("    weapon %p %s \"%s\"", w, Cls(w).name.c_str(), Weapon(w).name.c_str());
    }
    return x;
}
// which of the shooter's weapons does value v belong to: the weapon itself, a pointer into it, or a value only it holds
static uint8_t *WeaponOfValue(const WIdx &x, uintptr_t v, const char **how) {
    // weapon objects lie next to each other (2026-11-18: 0x1710 apart), so a pointer into one belongs to the
    // closest weapon below it, not to the first one whose span covers it
    uint8_t *best = nullptr;
    for (uint8_t *w : x.list) {
        if ((uint8_t *)v == w) { *how = "weapon"; return w; }
        if (v > (uintptr_t)w && v < (uintptr_t)w + W_SPAN && (!best || w > best)) best = w;
    }
    if (best) { *how = "inside weapon"; return best; }
    auto it = x.uniq.find(v); if (it != x.uniq.end()) { *how = "value of weapon"; return it->second; }
    return nullptr;
}
// stack: address of the hook's return address; the callers' frames are above it. Closest evidence wins.
static uint8_t *WeaponOfHit(const uint8_t *stack, const uint8_t *info, uint8_t *shooter, uint64_t now, bool retry = false) {
    WIdx &x = WeaponsOf(shooter, now, retry);
    ULONG_PTR lo = 0, hi = 0; GetCurrentThreadStackLimits(&lo, &hi);
    const uint8_t *end = stack + 0x1000; if (hi && end > (const uint8_t *)hi) end = (const uint8_t *)hi;
    uint8_t *w = nullptr; const char *how = ""; char where[96] = "";
    // 1) the damage info itself
    for (size_t o = 0; o < 0xC0 && !w; o += 8)
        if ((w = WeaponOfValue(x, Rd<uintptr_t>(info + o), &how))) snprintf(where, sizeof where, "info+%zX", o);
    // 2) stack slots, and the records / objects they point to (bullets, BulletMtSystem entries ...)
    int reads = 0;
    for (const uint8_t *q = stack; q + 8 <= end && !w; q += 8) {
        uintptr_t v = *(const uintptr_t *)q;
        if (!HeapPtr(v)) continue;
        Kind k = K_OTHER; bool obj = GameObj(v, &k);
        if (obj && k == K_WEAPON) {                               // a weapon right on the stack (vehicle weapons do this)
            w = (uint8_t *)v; how = "weapon on stack"; snprintf(where, sizeof where, "stack+%lX", (long)(q - stack));
            if (std::find(x.list.begin(), x.list.end(), w) == x.list.end()) { x.list.push_back(w); x.builtMs = 0; }
            break;
        }
        if ((w = WeaponOfValue(x, v, &how))) { snprintf(where, sizeof where, "stack+%lX", (long)(q - stack)); break; }
        if (obj && k != K_BULLET) continue;                      // other game objects (soldiers, enemies, systems) are skipped
        if (reads++ >= 48) continue;
        uintptr_t rec[0x100]; size_t n = sizeof rec;              // 0x800 bytes of the record / bullet
        if (!SafeRead((const void *)v, rec, n)) { n = 0x100; if (!SafeRead((const void *)v, rec, n)) continue; }
        if (!obj) {                                               // plain memory: only a record of this shooter's bullet
            bool own = false;
            for (size_t i = 0; i < n / 8 && !own; ++i) own = (uint8_t *)rec[i] == shooter || (g_localSol && (uint8_t *)rec[i] == g_localSol);
            if (!own) continue;
            // a record that refers to two different weapons is a weapon list / weapon set, not a bullet
            uint8_t *first = nullptr; bool many = false; const char *hw;
            for (size_t i = 0; i < n / 8 && !many; ++i) { uint8_t *ww = WeaponOfValue(x, rec[i], &hw); if (ww) { if (first && ww != first) many = true; first = ww; } }
            if (many) continue;
        }
        for (size_t i = 0; i < n / 8; ++i) {
            Kind kr;
            if (obj && !WeaponOfValue(x, rec[i], &how) && GameObj(rec[i], &kr) && kr == K_WEAPON) {   // a bullet's direct link to a weapon not listed yet
                x.list.push_back((uint8_t *)rec[i]); x.builtMs = 0;
            }
            uint8_t *cand = WeaponOfValue(x, rec[i], &how);
            if (cand && !obj && (uintptr_t)cand == rec[i]) cand = nullptr;   // plain records: a bare weapon pointer proves nothing
            if ((w = cand)) {
                snprintf(where, sizeof where, "stack+%lX -> %s+%zX", (long)(q - stack), obj ? Cls((uint8_t *)v).name.c_str() : "record", i * 8);
                break;
            }
        }
    }
    if (!w && !retry && now - x.builtMs >= 500) return WeaponOfHit(stack, info, shooter, now, true);   // weapons changed? look again
    static std::unordered_map<uint8_t *, int> probeBudget;        // first 12 hits of every weapon, and of "none"
    if (g_wprobeHits == 0) probeBudget.clear();
    if (g_wprobe && g_wprobeHits < 600 && probeBudget[w]++ < 12) {
        ++g_wprobeHits;
        Log("probe hit by %s: %s via %s (%s); shooter has %zu weapon(s)", Cls(shooter).name.c_str(), WeaponName(w).c_str(), where[0] ? where : "-",
            how[0] ? how : "no match", x.list.size());
        if (!w) {                                                  // what was there instead
            std::string objs;
            for (const uint8_t *q = stack; q + 8 <= end && objs.size() < 600; q += 8) {
                uintptr_t v = *(const uintptr_t *)q; Kind k;
                if (GameObj(v, &k)) { char b[96]; snprintf(b, sizeof b, " +%lX=%s", (long)(q - stack), Cls((uint8_t *)v).name.c_str()); objs += b; }
            }
            Log("    stack objects:%s", objs.empty() ? " none" : objs.c_str());
        }
    }
    return w;
}

// ---- summary: one player's numbers, the unit sent over the network and written to reports ----
#pragma pack(push, 1)
struct EnemyRow { char name[40]; float dmg; uint32_t kills; };
struct WeaponRow { char name[48]; float dmg, best, peak1, peak5, active; uint32_t hits, kills; uint8_t veh; };
struct FfRow { char cls[16]; float dmg; };
struct Summary {
    char name[32];            // player name (UTF-8), from EDFStats.ini Name=
    char cls[16];             // Ranger / Wing Diver / Fencer / Air Raider
    float seconds;
    float dmg, dmgVeh, best, peak1, peak5, ffNpc, taken, takenVeh, dist, distVeh;
    float ffPlayers, selfDmg, takenFF;
    uint32_t hits, kills, killsVeh, parts, ffKills, deaths;
    uint8_t nEnemy; EnemyRow enemy[8];
    uint8_t nFfFrom; FfRow ffFrom[4];   // friendly fire this player received from other players
    uint8_t nWeapon; WeaponRow weapon[6];
};
struct Packet { uint32_t magic, version, sender, seq, mission; Summary s; };
#pragma pack(pop)
static const uint32_t MAGIC = 0x31534445;   // "EDS1"
static const uint32_t PROTO = 11;   // 10: enemy rows carry class names; 11: per weapon rows, no healing / rescues

static std::string g_name;                  // UTF-8
static void CopyStr(char *dst, size_t n, const std::string &s) {
    size_t k = std::min(s.size(), n - 1);
    while (k > 0 && k < s.size() && ((unsigned char)s[k] & 0xC0) == 0x80) --k;   // do not cut a UTF-8 char
    memcpy(dst, s.data(), k); dst[k] = 0;
}
static Summary MakeSummary(const PStats &p, const std::string &name) {
    Summary s{};
    CopyStr(s.name, sizeof s.name, name); CopyStr(s.cls, sizeof s.cls, ClassName(p.cls));
    s.seconds = (float)((GetTickCount64() - g_missionStartMs) / 1000.0);
    s.dmg = (float)p.dmg; s.dmgVeh = (float)p.dmgVeh; s.best = (float)p.best; s.peak1 = (float)p.dps.peak1;
    s.peak5 = (float)p.dps.peak5; s.ffNpc = (float)p.ffNpc; s.taken = (float)p.taken; s.takenVeh = (float)p.takenVeh;
    s.ffPlayers = (float)p.ffPlayersSeen; s.selfDmg = (float)p.selfDmg; s.takenFF = (float)p.takenFF;
    for (auto &f : p.ffFrom) {
        if (s.nFfFrom >= 4) break;
        FfRow &r = s.ffFrom[s.nFfFrom++]; CopyStr(r.cls, sizeof r.cls, f.first); r.dmg = (float)f.second;
    }
    s.dist = (float)p.dist; s.distVeh = (float)p.distVeh;
    s.hits = p.hits; s.kills = p.kills; s.killsVeh = p.killsVeh; s.parts = p.parts; s.ffKills = p.ffKills; s.deaths = p.deaths;
    std::vector<std::pair<double, std::string>> e;
    for (auto &d : p.dmgBy) e.push_back({ d.second, d.first });
    for (auto &k : p.killsBy) if (!p.dmgBy.count(k.first)) e.push_back({ 0.0, k.first });
    std::sort(e.begin(), e.end(), [](auto &a, auto &b) { return a.first > b.first; });
    for (auto &x : e) {
        if (s.nEnemy >= 8) break;
        EnemyRow &r = s.enemy[s.nEnemy++];
        CopyStr(r.name, sizeof r.name, x.second); r.dmg = (float)x.first;
        auto k = p.killsBy.find(x.second); r.kills = k != p.killsBy.end() ? k->second : 0;
    }
    // weapons: objects with the same name are one weapon (a weapon object can be recreated, e.g. after death)
    struct WSum { double dmg = 0, best = 0, p1 = 0, p5 = 0, active = 0; unsigned hits = 0, kills = 0; bool veh = false; };
    std::map<std::string, WSum> byName;
    for (auto &w : p.weapons) {
        WSum &t = byName[WeaponName(w.first)];
        t.dmg += w.second.dmg; t.best = std::max(t.best, w.second.best); t.hits += w.second.hits; t.kills += w.second.kills;
        t.p1 = std::max(t.p1, w.second.dps.peak1); t.p5 = std::max(t.p5, w.second.dps.peak5); t.active += w.second.activeSecs;
        t.veh |= w.second.veh;
    }
    std::vector<std::pair<std::string, WSum>> ws(byName.begin(), byName.end());
    std::sort(ws.begin(), ws.end(), [](auto &a, auto &b) { return a.second.dmg > b.second.dmg; });
    if (ws.size() > 6) {                     // the rest goes into one row
        WSum rest; for (size_t i = 5; i < ws.size(); ++i) {
            auto &t = ws[i].second; rest.dmg += t.dmg; rest.best = std::max(rest.best, t.best); rest.hits += t.hits; rest.kills += t.kills;
            rest.p1 = std::max(rest.p1, t.p1); rest.p5 = std::max(rest.p5, t.p5); rest.active += t.active;
        }
        ws.resize(5); ws.push_back({ L_(u8"Остальное оружие", u8"Other weapons"), rest });
    }
    for (auto &x : ws) {
        WeaponRow &r = s.weapon[s.nWeapon++];
        CopyStr(r.name, sizeof r.name, x.first); r.dmg = (float)x.second.dmg; r.best = (float)x.second.best;
        r.peak1 = (float)x.second.p1; r.peak5 = (float)x.second.p5; r.active = (float)x.second.active;
        r.hits = x.second.hits; r.kills = x.second.kills; r.veh = x.second.veh;
    }
    return s;
}

// ---- network ----------------------------------------------------------------------------------
static SOCKET g_sock = INVALID_SOCKET;
static int g_port = 27787, g_sendMs = 1000;
static std::vector<sockaddr_in> g_peers;
static uint32_t g_self, g_seq;
static unsigned g_rxEos, g_rxUdp, g_txEos, g_txUdp;   // packet counters, written with the results
struct Remote {
    std::string ip; uint64_t lastRecvMs = 0; uint32_t lastSeq = 0;
    std::map<uint32_t, Summary> byMission;                       // latest summary of each of the sender's missions
    std::map<uint32_t, std::pair<uint64_t, uint64_t>> span;      // when packets of that mission arrived (first, last)
};
static std::map<uint32_t, Remote> g_remotes;            // guarded by g_cs

static void ParsePeers(const std::string &s, bool usePorts) {
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find_first_of(",; ", i); if (j == std::string::npos) j = s.size();
        std::string item = s.substr(i, j - i); i = j + 1;
        if (item.empty()) continue;
        std::string host = item; int port = g_port;
        size_t c = item.rfind(':');
        if (c != std::string::npos) { host = item.substr(0, c); if (usePorts) port = atoi(item.c_str() + c + 1); }
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
        if (inet_pton(AF_INET, host.c_str(), &a.sin_addr) == 1) { g_peers.push_back(a); Log("peer %s:%d", host.c_str(), port); }
        else Log("WARNING: bad peer address '%s'", item.c_str());
    }
}
static bool NetInit() {
    WSADATA w; if (WSAStartup(MAKEWORD(2, 2), &w)) { Log("WARNING: WSAStartup failed, no stats sharing"); return false; }
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) { Log("WARNING: socket %d, no stats sharing", WSAGetLastError()); return false; }
    u_long nb = 1; ioctlsocket(g_sock, FIONBIO, &nb);
    BOOL no = FALSE; DWORD ret = 0;   // ignore ICMP "port unreachable" from peers that are not running yet
    WSAIoctl(g_sock, _WSAIOW(IOC_VENDOR, 12) /* SIO_UDP_CONNRESET */, &no, sizeof no, nullptr, 0, &ret, nullptr, nullptr);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)g_port); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_sock, (sockaddr *)&a, sizeof a)) {
        Log("WARNING: UDP port %d busy (%d), no stats sharing", g_port, WSAGetLastError());
        closesocket(g_sock); g_sock = INVALID_SOCKET; return false;
    }
    Log("UDP port %d open, %zu peer(s)", g_port, g_peers.size());
    return true;
}
static bool BuildPacket(Packet &k) {   // caller holds g_cs
    if (!g_localSol) return false;
    memset(&k, 0, sizeof k);
    k.magic = MAGIC; k.version = PROTO; k.sender = g_self; k.seq = ++g_seq; k.mission = g_missionId;
    k.s = MakeSummary(P(g_localSol), g_name);
    return true;
}
static void NetSend() {                // caller holds g_cs
    if (g_sock == INVALID_SOCKET || g_peers.empty()) return;
    Packet k; if (!BuildPacket(k)) return;
    for (auto &a : g_peers) if (sendto(g_sock, (const char *)&k, sizeof k, 0, (const sockaddr *)&a, sizeof a) == sizeof k) ++g_txUdp;
}
// one stats packet from another player, from either transport. caller holds g_cs
static void OnPacket(const void *data, size_t n, const char *via) {
    if (n >= 8 && ((const uint32_t *)data)[0] == MAGIC && (((const uint32_t *)data)[1] != PROTO || n != sizeof(Packet))) {
        static std::set<std::string> warned;
        if (warned.insert(via).second)
            Log("WARNING: stats from %s use another EDFStats version (protocol %u, %u bytes; this PC: protocol %u). "
                "Everybody needs the same EDFStats version", via, ((const uint32_t *)data)[1], (unsigned)n, PROTO);
        return;
    }
    if (n != sizeof(Packet)) return;
    Packet k; memcpy(&k, data, sizeof k);
    if (k.magic != MAGIC || k.version != PROTO || k.sender == g_self) return;
    k.s.name[sizeof k.s.name - 1] = 0; k.s.cls[sizeof k.s.cls - 1] = 0;
    if (k.s.nEnemy > 8) k.s.nEnemy = 8;
    if (k.s.nWeapon > 6) k.s.nWeapon = 6;
    for (auto &w : k.s.weapon) w.name[sizeof w.name - 1] = 0;
    if (k.s.nFfFrom > 4) k.s.nFfFrom = 4;
    for (auto &e : k.s.ffFrom) e.cls[sizeof e.cls - 1] = 0;
    if (!strncmp(via, "EOS", 3)) ++g_rxEos; else ++g_rxUdp;
    for (auto &e : k.s.enemy) e.name[sizeof e.name - 1] = 0;
    Remote &r = g_remotes[k.sender];
    if (r.lastRecvMs == 0) { r.ip = via; Log("stats from %s (%s, %s)", via, k.s.name, k.s.cls); }
    if (k.seq <= r.lastSeq && r.lastSeq - k.seq < 100000) return;     // duplicate (both transports) or old
    r.lastSeq = k.seq; r.lastRecvMs = GetTickCount64();
    auto sp = r.span.find(k.mission);
    if (sp == r.span.end()) r.span[k.mission] = { r.lastRecvMs, r.lastRecvMs }; else sp->second.second = r.lastRecvMs;
    r.byMission[k.mission] = k.s;
    while (r.byMission.size() > 4) { r.span.erase(r.byMission.begin()->first); r.byMission.erase(r.byMission.begin()); }
}
static void NetReceive() {             // caller holds g_cs
    if (g_sock == INVALID_SOCKET) return;
    for (;;) {
        Packet k; sockaddr_in from; int fl = sizeof from;
        int n = recvfrom(g_sock, (char *)&k, sizeof k, 0, (sockaddr *)&from, &fl);
        if (n < 0) break;
        char ip[48]; inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        std::string via = std::string("UDP ") + ip;
        OnPacket(&k, (size_t)n, via.c_str());
    }
}
// for every remote player: the mission of theirs that overlaps our window [from, to] the most
// (players enter and leave missions a few seconds apart, so ids cannot be compared directly)
static std::vector<Summary> RemoteSummaries(uint64_t from, uint64_t to) {
    std::vector<Summary> out;
    for (auto &kv : g_remotes) {
        int64_t best = 0; const Summary *bs = nullptr;
        for (auto &m : kv.second.byMission) {
            auto &sp = kv.second.span[m.first];
            // each packet stands for the following send interval (+1.5 s)
            int64_t ov = (int64_t)std::min(sp.second, to) + 1500 - (int64_t)std::max(sp.first, from);
            if (ov > best) { best = ov; bs = &m.second; }
        }
        if (bs) out.push_back(*bs);
    }
    return out;
}

// ---- transport through the game's own EOS P2P connection ----------------------------------------
// The game polls EOS_P2P_ReceivePacket without a channel filter and accepts connections only on its own
// socket (EDF.dll+12C3560), so: we accept our socket "EDFStats" ourselves, take our packets out of the
// game's receive calls before it sees them, and send from inside those calls (EOS is not thread safe and
// must be used from the game's thread). Players without the plugin never accept our socket, their game
// does not see our packets.
typedef void *EOS_HP2P; typedef void *EOS_PUID;
struct EOS_SocketId { int32_t ApiVersion; char SocketName[33]; };
struct EOS_SendOpts { int32_t ApiVersion; EOS_PUID LocalUserId, RemoteUserId; const EOS_SocketId *SocketId; uint8_t Channel;
                      uint32_t DataLengthBytes; const void *Data; int32_t bAllowDelayedDelivery, Reliability, bDisableAutoAcceptConnection; };
struct EOS_RecvOpts { int32_t ApiVersion; EOS_PUID LocalUserId; uint32_t MaxDataSizeBytes; const uint8_t *RequestedChannel; };
struct EOS_AcceptOpts { int32_t ApiVersion; EOS_PUID LocalUserId, RemoteUserId; const EOS_SocketId *SocketId; };
// same layout the game uses (EDF.dll+12C4FE7: ApiVersion 3, channel at +0x20, auto-accept flag at +0x38)
static_assert(offsetof(EOS_SendOpts, Channel) == 0x20 && offsetof(EOS_SendOpts, bDisableAutoAcceptConnection) == 0x38, "EOS layout");
static_assert(sizeof(Packet) < 1170, "must fit one EOS packet");
typedef int32_t (*EosSendFn)(EOS_HP2P, const EOS_SendOpts *);
typedef int32_t (*EosRecvFn)(EOS_HP2P, const EOS_RecvOpts *, EOS_PUID *, EOS_SocketId *, uint8_t *, void *, uint32_t *);
typedef int32_t (*EosAcceptFn)(EOS_HP2P, const EOS_AcceptOpts *);
typedef int32_t (*EosIdToStrFn)(EOS_PUID, char *, int32_t *);
static EosSendFn oEosSend; static EosRecvFn oEosRecv; static EosAcceptFn pEosAccept; static EosIdToStrFn pEosIdToStr;
static bool g_useEos = true;
static const char *EOS_SOCKET = "EDFStats";
static const uint8_t EOS_CHANNEL = 7;
static EOS_SocketId g_eosSock;
static EOS_HP2P g_eosP2P; static EOS_PUID g_eosLocal;
struct EosPeer { uint64_t lastGameMs = 0, lastSendMs = 0, firstSendMs = 0, lastHeardMs = 0; bool accepted = false; std::string id; };
static std::map<EOS_PUID, EosPeer> g_eosPeers;          // guarded by g_eosCs
static CRITICAL_SECTION g_eosCs;
static uint64_t g_eosLastPump;

static std::string EosIdStr(EOS_PUID id) {
    char b[64] = {}; int32_t n = sizeof b;
    if (pEosIdToStr && pEosIdToStr(id, b, &n) == 0) return std::string(b).substr(0, 8);
    return "?";
}
static void EosPump(uint64_t now) {     // game thread, g_eosCs held
    if (!g_eosP2P || !g_eosLocal || now - g_eosLastPump < 100) return;
    g_eosLastPump = now;
    Packet k; bool have = false;
    for (auto &kv : g_eosPeers) {
        EosPeer &pr = kv.second;
        if (now - pr.lastGameMs > 60000) continue;          // the game no longer talks to this player
        if (!pr.accepted && pEosAccept) {
            EOS_AcceptOpts a{ 1, g_eosLocal, kv.first, &g_eosSock };
            int32_t r = pEosAccept(g_eosP2P, &a);
            pr.accepted = true; pr.id = EosIdStr(kv.first);
            Log("EOS: player %s, our socket accepted (result %d)", pr.id.c_str(), r);
        }
        // a player that never answered (no plugin) gets a packet every 10 s instead of every second
        uint64_t every = (pr.lastHeardMs == 0 && pr.firstSendMs && now - pr.firstSendMs > 30000) ? 10000 : (uint64_t)g_sendMs;
        if (now - pr.lastSendMs < every) continue;
        if (!have) {
            EnterCriticalSection(&g_cs); have = BuildPacket(k); LeaveCriticalSection(&g_cs);
            if (!have) return;                               // nothing to report before our first hit
        }
        EOS_SendOpts o{}; o.ApiVersion = 3; o.LocalUserId = g_eosLocal; o.RemoteUserId = kv.first; o.SocketId = &g_eosSock;
        o.Channel = EOS_CHANNEL; o.DataLengthBytes = sizeof k; o.Data = &k; o.bAllowDelayedDelivery = 1;
        o.Reliability = 0;                                   // unreliable: a fresh packet follows every second
        o.bDisableAutoAcceptConnection = 0;
        int32_t r = oEosSend(g_eosP2P, &o);
        if (r == 0) ++g_txEos;
        if (!pr.firstSendMs) Log("EOS: first stats packet to %s, result %d", pr.id.c_str(), r);
        pr.lastSendMs = now; if (!pr.firstSendMs) pr.firstSendMs = now;
    }
}
static int32_t hEosSend(EOS_HP2P h, const EOS_SendOpts *o) {
    if (g_useEos && o && o->RemoteUserId) {
        EnterCriticalSection(&g_eosCs);
        g_eosP2P = h; g_eosLocal = o->LocalUserId;
        g_eosPeers[o->RemoteUserId].lastGameMs = GetTickCount64();
        LeaveCriticalSection(&g_eosCs);
    }
    return oEosSend(h, o);
}
static int32_t hEosRecv(EOS_HP2P h, const EOS_RecvOpts *o, EOS_PUID *peer, EOS_SocketId *sock, uint8_t *ch, void *data, uint32_t *bytes) {
    if (!g_useEos) return oEosRecv(h, o, peer, sock, ch, data, bytes);
    uint64_t now = GetTickCount64();
    EnterCriticalSection(&g_eosCs);
    if (o) { g_eosP2P = h; g_eosLocal = o->LocalUserId; }
    EosPump(now);
    LeaveCriticalSection(&g_eosCs);
    for (;;) {
        int32_t r = oEosRecv(h, o, peer, sock, ch, data, bytes);
        if (r != 0 || !sock || !peer || !bytes || strncmp(sock->SocketName, EOS_SOCKET, sizeof sock->SocketName) != 0) return r;
        // ours: give it to the stats and fetch the next packet for the game
        EnterCriticalSection(&g_eosCs);
        EosPeer &pr = g_eosPeers[*peer];
        if (pr.id.empty()) pr.id = EosIdStr(*peer);
        pr.lastHeardMs = now;
        std::string via = "EOS " + pr.id;
        LeaveCriticalSection(&g_eosCs);
        EnterCriticalSection(&g_cs); OnPacket(data, *bytes, via.c_str()); LeaveCriticalSection(&g_cs);
    }
}
static void **FindIat(uint8_t *mod, const char *dllPart, const char *func) {
    auto nt = (IMAGE_NT_HEADERS *)(mod + ((IMAGE_DOS_HEADER *)mod)->e_lfanew);
    auto &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR *)(mod + dir.VirtualAddress); imp->Name; ++imp) {
        std::string dn = (const char *)(mod + imp->Name);
        std::transform(dn.begin(), dn.end(), dn.begin(), [](char c) { return (char)tolower((unsigned char)c); });
        if (dn.find(dllPart) == std::string::npos) continue;
        auto names = (IMAGE_THUNK_DATA *)(mod + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto iat = (IMAGE_THUNK_DATA *)(mod + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++iat) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME *)(mod + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, func) == 0) return (void **)&iat->u1.Function;
        }
    }
    return nullptr;
}
static bool PatchIat(void **slot, void *fn, void **orig) {
    DWORD old;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old)) return false;
    *orig = *slot; *slot = fn;
    VirtualProtect(slot, sizeof(void *), old, &old);
    return true;
}
static void EosInit() {
    InitializeCriticalSection(&g_eosCs);
    if (!g_useEos) { Log("sharing through the game connection is off (ShareViaGame=0)"); return; }
    g_eosSock.ApiVersion = 1; strcpy(g_eosSock.SocketName, EOS_SOCKET);
    void **sSend = FindIat(g_base, "eossdk", "EOS_P2P_SendPacket"), **sRecv = FindIat(g_base, "eossdk", "EOS_P2P_ReceivePacket");
    void **sAcc = FindIat(g_base, "eossdk", "EOS_P2P_AcceptConnection"), **sStr = FindIat(g_base, "eossdk", "EOS_ProductUserId_ToString");
    if (!sSend || !sRecv || !sAcc) { Log("WARNING: EOS imports not found, sharing only over UDP"); g_useEos = false; return; }
    pEosAccept = (EosAcceptFn)*sAcc; pEosIdToStr = sStr ? (EosIdToStrFn)*sStr : nullptr;
    if (!PatchIat(sSend, (void *)hEosSend, (void **)&oEosSend) || !PatchIat(sRecv, (void *)hEosRecv, (void **)&oEosRecv)) {
        Log("WARNING: could not patch EOS imports, sharing only over UDP"); g_useEos = false; return;
    }
    Log("sharing through the game's EOS connection enabled (socket %s)", EOS_SOCKET);
}

// ---- player name from Steam ------------------------------------------------------------------------
static bool g_nameFromIni; static int g_steamTries;
static void SteamName() {               // worker thread, caller holds g_cs
    if (g_nameFromIni || !g_name.empty() || g_steamTries > 60) return;
    ++g_steamTries;
    HMODULE st = GetModuleHandleW(L"steam_api64.dll");
    if (!st) return;
    typedef void *(*GetFriendsFn)(); typedef const char *(*GetPersonaFn)(void *);
    auto gf = (GetFriendsFn)GetProcAddress(st, "SteamAPI_SteamFriends_v017");
    auto gp = (GetPersonaFn)GetProcAddress(st, "SteamAPI_ISteamFriends_GetPersonaName");
    if (!gf || !gp) { g_steamTries = 999; Log("Steam name: functions not found, set Name= in EDFStats.ini"); return; }
    void *fr = gf(); if (!fr) return;
    const char *n = gp(fr);
    if (n && *n) { g_name = n; Log("player name from Steam: %s", n); }
}

// ---- mission handling ---------------------------------------------------------------------------
static void WriteResults(const char *why);
static bool g_missionEnded, g_autoBoardOnEnd = true; static uint64_t g_finalWriteMs;
static volatile bool g_autoBoard;      // scoreboard shown by itself on the result screen
static uint64_t g_lastLocalMs;          // last hit this machine's player took part in
static uint64_t g_lastFightMs;           // last hit any player took part in
struct SlotInfo { uint8_t *obj = nullptr; uint64_t createdMs = 0; };
static std::map<unsigned, SlotInfo> g_slots;            // player slot -> its current character (cleared at mission end)
// the game sometimes replaces this machine's player object in the middle of a mission
// (seen 2026-11-14 about 2 min in, the other player's mission went on): keep counting on the new object
static void RebindLocal(uint8_t *local) {
    auto it = g_ps.find(g_localSol);
    if (it != g_ps.end()) { PStats moved = std::move(it->second); g_ps.erase(it); g_ps[local] = std::move(moved); }
    Log("this PC's player object was replaced (%p -> %p) in the middle of the mission, stats kept", g_localSol, local);
    g_localSol = local;
}
static void NewMission(uint8_t *local) {
    if (g_localSol && !g_ps.empty() && !g_missionEnded) WriteResults("new mission");
    g_missionEnded = false; g_finalWriteMs = 0;
    g_ps.clear(); g_localSol = local; g_hitLines = 0; ++g_missionId; g_weapons.clear(); g_widx.clear(); g_weaponNo = 0; g_wprobeHits = 0;
    g_missionStartMs = GetTickCount64(); GetLocalTime(&g_missionStart);
    Log("---- new mission (this PC: %s) ----", local ? ClassName(Cls(local).name) : "?");
    NetSend();
}

// ---- mission end: the game builds its result screen ---------------------------------------------
typedef void *(__fastcall *ResultCtorFn)(void *self, void *init);
static ResultCtorFn oResultCtor;
static void *__fastcall hResultCtor(void *self, void *init) {
    void *r = oResultCtor(self, init);
    EnterCriticalSection(&g_cs);
    if (g_localSol && !g_missionEnded) {
        g_missionEnded = true;
        g_slots.clear();
        Log("---- mission result screen ----");
        NetSend();
        g_finalWriteMs = GetTickCount64() + 3000;   // write once the other players' final numbers have arrived
    }
    LeaveCriticalSection(&g_cs);
    return r;
}

typedef void *(__fastcall *ResultDtorFn)(void *self, int flags);
static ResultDtorFn oResultDtor;
static void *__fastcall hResultDtor(void *self, int flags) {
    g_autoBoard = false;
    return oResultDtor(self, flags);
}

// ---- player spawns: mission start, and a player object replaced in the middle of a mission -------
typedef uint8_t *(__fastcall *CreatePlayerFn)(unsigned slot, const float *where, int equip);
static CreatePlayerFn oCreatePlayer;
static uint8_t *__fastcall hCreatePlayer(unsigned slot, const float *where, int equip) {
    uint8_t *obj = oCreatePlayer(slot, where, equip);
    float pos[3] = {};
    if (where) SafeRead(where + 12, pos, sizeof pos);          // row 3 of the 4x4 matrix = position
    uint64_t now = GetTickCount64();
    EnterCriticalSection(&g_cs);
    const ClsInfo &ci = Cls(obj);
    // nobody has fought for 90 s: this is a new mission (left without a result screen)
    if (now - g_lastFightMs > 90000) g_slots.clear();
    SlotInfo &si = g_slots[slot];
    uint8_t *prev = si.obj;
    Log("player spawned: slot %u %s at (%.0f %.0f %.0f) -> %p, previous %p", slot, ClassName(ci.name), pos[0], pos[1], pos[2], obj, prev);
    if (prev && prev != obj && !g_missionEnded) {
        if (prev == g_localSol) RebindLocal(obj);           // keep counting on the new character
        else if (g_ps.count(prev)) { PStats moved = std::move(g_ps[prev]); g_ps.erase(prev); g_ps[obj] = std::move(moved); }
    }
    si.obj = obj; si.createdMs = now;
    LeaveCriticalSection(&g_cs);
    return obj;
}

// ---- hook -------------------------------------------------------------------------------------
typedef void *(__fastcall *ApplyDamageFn)(uint8_t *self, uint8_t *info);
static ApplyDamageFn oApplyDamage;

static const uint8_t *g_hitStack;        // set by hApplyDamage for the hit being handled
static void OnHit(uint8_t *self, uint8_t *info, float hp0, float hp1, bool dead0, bool dead1, int tTeam) {
    float delta = hp0 - hp1;                                // > 0 damage, < 0 healing actually applied
    if (delta == 0 && !(dead1 && !dead0)) return;
    uint8_t *atk = Rd<uint8_t *>(info + DI_ATTACKER);
    int iTeam = Rd<int32_t>(info + DI_TEAM, -1);
    bool killed = !dead0 && dead1;
    uint64_t now = GetTickCount64();
    const ClsInfo &tc = Cls(self);
    const ClsInfo &acls = Cls(atk);

    uint8_t *who = nullptr; bool viaVeh = false;           // attacker: player on foot or player's vehicle
    if (acls.kind == K_SOLDIER && IsPlayer(atk)) who = atk;
    else if (acls.kind == K_VEHICLE) { who = Driver(atk); viaVeh = who != nullptr; }
    uint8_t *victim = nullptr; bool victimVeh = false;     // victim: player, or vehicle driven by one
    if (tc.kind == K_SOLDIER && IsPlayer(self)) victim = self;
    else if (tc.kind == K_VEHICLE) { victim = Driver(self); victimVeh = victim != nullptr; }

    uint8_t *loc = nullptr;                                 // this machine's player object changed -> new mission
    if (who && IsLocal(who)) loc = who; else if (victim && IsLocal(victim)) loc = victim;
    if (loc && loc != g_localSol) {
        // a new mission only after the result screen, at the very start, or after a long quiet time (left to the menu)
        if (!g_localSol || g_missionEnded || now - g_lastLocalMs > 90000) NewMission(loc); else RebindLocal(loc);
    }
    if (loc) g_lastLocalMs = now;
    if (who || victim) g_lastFightMs = now;
    if (!who && !victim) return;

    int fr = Friendly(iTeam, tTeam);
    if (fr < 0) fr = (iTeam >= 0 && iTeam == tTeam) ? 1 : 0;
    if (tc.kind == K_VEHICLE) fr = 1;        // Vehicle* objects are EDF vehicles (an empty one has no team): never an enemy
    bool mine = who && IsLocal(who);         // weapons are told apart on the shooter's own PC

    if (who) {
        PStats &p = P(who);
        bool selfHit = self == who || (viaVeh && self == atk) || victim == who;   // own body or own vehicle
        if (delta > 0 && selfHit) p.selfDmg += delta;
        else if (delta > 0) {
            if (fr && victim) p.ffPlayersSeen += delta;                     // another player (rarely visible here)
            else if (fr) { p.ffNpc += delta; if (killed) ++p.ffKills; }     // allied NPC soldier / NPC vehicle
            else {
                ++p.hits; p.dmg += delta; p.best = std::max(p.best, (double)delta); p.dps.Add(now, delta);
                p.dmgBy[tc.name] += delta;
                if (viaVeh) p.dmgVeh += delta;
                bool kill = killed && !IsPart(tc.name);
                if (killed && IsPart(tc.name)) ++p.parts;
                else if (killed) { ++p.kills; ++p.killsBy[tc.name]; if (viaVeh) ++p.killsVeh; }
                if (mine && g_hitStack) {
                    g_hitOwner = viaVeh ? atk : who;
                    uint8_t *wo = WeaponOfHit(g_hitStack, info, g_hitOwner, now);
                    if (wo) Weapon(wo);                     // name it now, while the object is alive
                    WStats &w = p.weapons[wo]; w.Add(now, delta, kill); if (wo) w.veh |= viaVeh;
                    g_hitOwner = nullptr;
                }
            }
        } else if (killed && !fr && delta == 0) {          // kill whose HP change happened on another machine
            if (IsPart(tc.name)) ++p.parts; else { ++p.kills; ++p.killsBy[tc.name]; }
        }
    }
    if (victim && delta > 0) {
        PStats &v = P(victim);
        if (who == victim) {}                                   // own fire: counted as selfDmg above
        else if (fr) { v.takenFF += delta; if (who) v.ffFrom[ClassName(Cls(who).name)] += delta; }
        else if (victimVeh) v.takenVeh += delta; else v.taken += delta;
        if (killed && !victimVeh) ++v.deaths;
    } else if (victim && killed && !victimVeh) ++P(victim).deaths;

    if (g_logHits && g_hitLines++ < 50000)
        LogHit("HIT tgt=%p %s hp %.1f->%.1f | atk=%p %s who=%p%s fr=%d raw=%.2f applied=%.2f flags=%04X%s",
               self, tc.name.c_str(), hp0, hp1, atk, acls.name.c_str(), who, viaVeh ? " (vehicle)" : "", fr,
               Rd<float>(info + DI_DAMAGE), delta, Rd<uint16_t>(info + DI_FLAGS), killed ? " KILL" : "");
}

static void *__fastcall hApplyDamage(uint8_t *self, uint8_t *info) {
    float hp0 = Rd<float>(self + GO_HP); uint8_t dead0 = Rd<uint8_t>(self + GO_DEAD);
    int tTeam = Rd<int32_t>(self + GO_TEAM, -1);           // before: dying objects switch to team -1
    void *r = oApplyDamage(self, info);
    float hp1 = Rd<float>(self + GO_HP); uint8_t dead1 = Rd<uint8_t>(self + GO_DEAD);
    if (hp0 == hp1 && dead0 == dead1) return r;            // fast path: nothing changed here
    const uint8_t *stk = (const uint8_t *)_AddressOfReturnAddress();
    EnterCriticalSection(&g_cs);
    g_hitStack = stk;
    OnHit(self, info, hp0, hp1, dead0 != 0, dead1 != 0, tTeam);
    g_hitStack = nullptr;
    LeaveCriticalSection(&g_cs);
    return r;
}

// ---- distance (sampled from the worker thread every 100 ms) -------------------------------------
static float g_lastPos[3]; static uint8_t *g_lastPosSol;
static void SampleDistance() {             // caller holds g_cs
    uint8_t *s = g_localSol;
    float p[3];
    if (!s || !SafeRead(s + SB_POS, p, sizeof p) || !std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2])) {
        g_lastPosSol = nullptr; return;
    }
    if (g_lastPosSol == s) {
        float dx = p[0] - g_lastPos[0], dy = p[1] - g_lastPos[1], dz = p[2] - g_lastPos[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > 0.02f && d < 40.0f) {          // < 400 m/s: ignore teleports and respawns
            PStats &st = P(s);
            if (Rd<void *>(s + SB_VEHICLE)) st.distVeh += d; else st.dist += d;
        }
    }
    memcpy(g_lastPos, p, sizeof p); g_lastPosSol = s;
}

// ---- results ---------------------------------------------------------------------------------------
static std::string Esc(const std::string &s, bool html) {
    std::string o;
    for (char c : s) {
        if (html) {
            if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;"; else if (c == '"') o += "&quot;";
            else o += c;
        } else { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 0x20) o += c; }
    }
    return o;
}
static std::string Num(double v) {         // 12345.6 -> "12 346"
    char b[32]; snprintf(b, sizeof b, "%.0f", v);
    std::string s(b), o; int n = 0;
    for (int i = (int)s.size() - 1; i >= 0; --i) {
        o.insert(o.begin(), s[i]);
        if (isdigit((unsigned char)s[i]) && ++n % 3 == 0 && i > 0 && isdigit((unsigned char)s[i - 1])) o.insert(0, u8" ");
    }
    return o;
}
struct Row { Summary s; bool local; bool partial; };

static void WriteJson(const std::string &path, const std::vector<Row> &rows, const char *why, double secs) {
    FILE *f = fopen(path.c_str(), "w");
    if (!f) { Log("WARNING: cannot write %s", path.c_str()); return; }
    fprintf(f, "{\n  \"version\": \"%s\",\n  \"reason\": \"%s\",\n  \"seconds\": %.1f,\n  \"players\": [\n", VERSION, why, secs);
    for (size_t i = 0; i < rows.size(); ++i) {
        const Summary &s = rows[i].s;
        fprintf(f, "    {\"name\": \"%s\", \"class\": \"%s\", \"thisPC\": %s, \"partial\": %s, \"seconds\": %.1f, \"damage\": %.1f, "
                   "\"damageVehicle\": %.1f, \"hits\": %u, \"kills\": %u, \"killsVehicle\": %u, \"partsDestroyed\": %u, \"bestHit\": %.1f, "
                   "\"peakDps1s\": %.1f, \"peakDps5s\": %.1f, \"damageToPlayers\": %.1f, \"damageToAlliedNpc\": %.1f, "
                   "\"alliedNpcKilled\": %u, \"damageToSelf\": %.1f, \"takenFromAllies\": %.1f, \"taken\": %.1f, "
                   "\"takenVehicle\": %.1f, \"deaths\": %u, \"distance\": %.1f, \"distanceVehicle\": %.1f,\n"
                   "     \"byEnemy\": [",
                Esc(s.name, false).c_str(), Esc(s.cls, false).c_str(), rows[i].local ? "true" : "false", rows[i].partial ? "true" : "false",
                s.seconds, s.dmg, s.dmgVeh, s.hits, s.kills, s.killsVeh, s.parts, s.best, s.peak1, s.peak5, s.ffPlayers, s.ffNpc,
                s.ffKills, s.selfDmg, s.takenFF, s.taken,
                s.takenVeh, s.deaths, s.dist, s.distVeh);
        for (int e = 0; e < s.nEnemy; ++e)
            fprintf(f, "%s{\"enemy\": \"%s\", \"class\": \"%s\", \"damage\": %.1f, \"kills\": %u}", e ? ", " : "", Esc(EnemyName(s.enemy[e].name), false).c_str(), Esc(s.enemy[e].name, false).c_str(),
                    s.enemy[e].dmg, s.enemy[e].kills);
        fprintf(f, "],\n     \"byWeapon\": [");
        for (int w = 0; w < s.nWeapon; ++w) {
            const WeaponRow &x = s.weapon[w];
            fprintf(f, "%s{\"weapon\": \"%s\", \"damage\": %.1f, \"hits\": %u, \"kills\": %u, \"bestHit\": %.1f, \"peakDps1s\": %.1f, "
                       "\"peakDps5s\": %.1f, \"activeSeconds\": %.0f, \"avgDps\": %.1f, \"vehicle\": %s}", w ? ", " : "",
                    Esc(x.name, false).c_str(), x.dmg, x.hits, x.kills, x.best, x.peak1, x.peak5, x.active, x.active > 0 ? x.dmg / x.active : 0.0,
                    x.veh ? "true" : "false");
        }
        fprintf(f, "]}%s\n", i + 1 < rows.size() ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

static void WriteHtml(const std::string &path, const std::vector<Row> &rows, double secs) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) { Log("WARNING: cannot write %s", path.c_str()); return; }
    char when[64];
    snprintf(when, sizeof when, "%02d.%02d.%04d %02d:%02d", g_missionStart.wDay, g_missionStart.wMonth, g_missionStart.wYear,
             g_missionStart.wHour, g_missionStart.wMinute);
    auto nm = [&](const Row &r) { std::string n = r.s.name[0] ? r.s.name : r.s.cls; return Esc(n, true); };
    double top = 1; for (auto &r : rows) top = std::max(top, (double)r.s.dmg);

    std::string head = u8R"(<!doctype html><html lang="{LANG}"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>{TITLE}</title><style>
:root{--bg:#0d1210;--panel:#151d19;--line:#24312b;--text:#e7efe9;--dim:#8fa398;--acc:#e2b93b;--acc2:#5fd18b;--bad:#e06a55}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 "Segoe UI",system-ui,sans-serif;padding:24px 16px}
main{max-width:1100px;margin:0 auto}h1{font-size:26px;margin:0 0 4px;letter-spacing:.04em;text-transform:uppercase}
h1 b{color:var(--acc)}.sub{color:var(--dim);margin-bottom:22px}h2{font-size:15px;text-transform:uppercase;letter-spacing:.08em;color:var(--dim);margin:28px 0 10px}
.awards{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:10px}
.aw{background:var(--panel);border:1px solid var(--line);border-left:3px solid var(--acc);padding:10px 12px;border-radius:4px}
.aw .t{color:var(--acc);font-weight:600;font-size:13px;text-transform:uppercase;letter-spacing:.05em}.aw .w{font-size:17px;margin-top:2px}.aw .v{color:var(--dim);font-size:13px}
.wrap{overflow-x:auto;background:var(--panel);border:1px solid var(--line);border-radius:4px}
table{border-collapse:collapse;width:100%;min-width:760px}th,td{padding:8px 10px;text-align:right;white-space:nowrap;border-bottom:1px solid var(--line)}
th{color:var(--dim);font-weight:500;font-size:12px;text-transform:uppercase;letter-spacing:.04em}th:first-child,td:first-child{text-align:left}
tr:last-child td{border-bottom:0}.me td:first-child{color:var(--acc2)}.cls{color:var(--dim);font-size:12px}
.bar{height:4px;background:var(--line);margin-top:4px;border-radius:2px}.bar i{display:block;height:4px;background:var(--acc);border-radius:2px}
.cards{display:grid;grid-template-columns:repeat(auto-fill,minmax(320px,1fr));gap:12px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:4px;padding:12px 14px}.card h3{margin:0 0 8px;font-size:16px}
.cards.wide{grid-template-columns:1fr}.card table{min-width:0}.card td,.card th{padding:4px 6px}.card td:first-child{white-space:normal}.note{color:var(--dim);font-size:13px;margin-top:22px}.warn{color:var(--bad)}
</style></head><body><main>
)";
    head.replace(head.find("{LANG}"), 6, g_en ? "en" : "ru");
    head.replace(head.find("{TITLE}"), 7, L_(u8"EDF6 — итоги миссии", u8"EDF6 — mission results"));
    fputs(head.c_str(), f);
    fprintf(f, L_(u8"<h1>Итоги миссии <b>EDF6</b></h1><div class=\"sub\">%s · длительность %d:%02d · игроков: %zu</div>\n", u8"<h1>Mission results <b>EDF6</b></h1><div class=\"sub\">%s · duration %d:%02d · players: %zu</div>\n"),
            when, (int)secs / 60, (int)secs % 60, rows.size());

    // awards
    struct Aw { const char *title; double (*val)(const Summary &); const char *unit; };
    static const Aw aws[] = {
        { L_(u8"MVP — больше всего урона", u8"MVP — most damage"), [](const Summary &s) { return (double)s.dmg; }, "" },
        { L_(u8"Палач — больше всего убийств", u8"Executioner — most kills"), [](const Summary &s) { return (double)s.kills; }, "" },
        { L_(u8"Пиковая мощь — урон за 1 с", u8"Peak power — damage in 1 s"), [](const Summary &s) { return (double)s.peak1; }, "" },
        { L_(u8"Самый сильный удар", u8"Biggest hit"), [](const Summary &s) { return (double)s.best; }, "" },
        { L_(u8"Стена — принял урона от врагов", u8"Wall — damage taken from enemies"), [](const Summary &s) { return (double)(s.taken + s.takenVeh); }, "" },
        { L_(u8"Турист — прошёл пешком", u8"Tourist — distance on foot"), [](const Summary &s) { return (double)s.dist; }, L_(u8" м", u8" m") },
        { L_(u8"Водитель — урон с техники", u8"Driver — damage from vehicles"), [](const Summary &s) { return (double)s.dmgVeh; }, "" },
        { L_(u8"Предатель — урон по игрокам", u8"Traitor — damage to players"), [](const Summary &s) { return (double)s.ffPlayers; }, "" },
        { L_(u8"Гроза NPC — урон по союзным NPC", u8"NPC menace — damage to allied NPCs"), [](const Summary &s) { return (double)s.ffNpc; }, "" },
        { L_(u8"Сапёр — урон по себе", u8"Sapper — damage to yourself"), [](const Summary &s) { return (double)s.selfDmg; }, "" },
    };
    fputs(L_(u8"<h2>Награды</h2><div class=\"awards\">\n", u8"<h2>Awards</h2><div class=\"awards\">\n"), f);
    for (auto &a : aws) {
        int bi = -1; double bv = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            double v = a.val(rows[i].s);
            if (rows[i].partial) continue;
            if (bi < 0 || v > bv) { bi = (int)i; bv = v; }
        }
        if (bi < 0 || bv <= 0) continue;
        fprintf(f, "<div class=\"aw\"><div class=\"t\">%s</div><div class=\"w\">%s</div><div class=\"v\">%s%s</div></div>\n",
                a.title, nm(rows[bi]).c_str(), Num(bv).c_str(), a.unit);
    }
    fputs("</div>\n", f);

    // main table
    fputs(L_(u8"<h2>Таблица</h2><div class=\"wrap\"><table><tr><th>Игрок</th><th>Урон</th><th>Пик DPS 1с / 5с</th><th>Убийства</th>", u8"<h2>Scoreboard</h2><div class=\"wrap\"><table><tr><th>Player</th><th>Damage</th><th>Peak DPS 1s / 5s</th><th>Kills</th>"), f);
    fputs(L_(u8"<th>Части</th><th>Лучший удар</th><th>Получил: враги / свои</th><th>Смерти</th>", u8"<th>Parts</th><th>Best hit</th><th>Taken: enemies / allies</th><th>Deaths</th>"), f);
    fputs(L_(u8"<th>По игрокам</th><th>По NPC</th><th>Себе</th><th>Пешком</th><th>На технике</th></tr>\n", u8"<th>To players</th><th>To NPCs</th><th>To self</th><th>On foot</th><th>In vehicles</th></tr>\n"), f);
    for (auto &r : rows) {
        const Summary &s = r.s;
        fprintf(f, "<tr%s><td>%s <span class=\"cls\">%s%s</span><div class=\"bar\"><i style=\"width:%.1f%%\"></i></div></td>",
                r.local ? " class=\"me\"" : "", nm(r).c_str(), Esc(s.cls, true).c_str(),
                r.partial ? L_(u8" · неполные данные", u8" · partial data") : "", 100.0 * s.dmg / top);
        std::string rowFmt = std::string("<td>%s</td><td>%s / %s</td><td>%u</td><td>%u</td><td>%s</td><td>%s / %s</td><td>%u</td>") + L_(u8"<td>%s</td><td>%s</td><td>%s</td><td>%s м</td><td>%s м</td></tr>\n", u8"<td>%s</td><td>%s</td><td>%s</td><td>%s m</td><td>%s m</td></tr>\n");
        fprintf(f, rowFmt.c_str(),
                Num(s.dmg).c_str(), Num(s.peak1).c_str(), Num(s.peak5).c_str(), s.kills, s.parts, Num(s.best).c_str(),
                Num(s.taken + s.takenVeh).c_str(), Num(s.takenFF).c_str(), s.deaths, Num(s.ffPlayers).c_str(),
                Num(s.ffNpc).c_str(), Num(s.selfDmg).c_str(), Num(s.dist).c_str(), Num(s.distVeh).c_str());
    }
    fputs("</table></div>\n", f);

    // per weapon
    bool anyW = false; for (auto &r : rows) anyW |= r.s.nWeapon > 0;
    if (anyW) {
        fputs(L_(u8"<h2>По оружию</h2><div class=\"cards wide\">\n", u8"<h2>By weapon</h2><div class=\"cards wide\">\n"), f);
        for (auto &r : rows) {
            const Summary &s = r.s;
            if (!s.nWeapon) continue;
            double tot = 0; for (int w = 0; w < s.nWeapon; ++w) tot += s.weapon[w].dmg;
            fprintf(f, "<div class=\"card\"><h3>%s <span class=\"cls\">%s</span></h3><div class=\"wrap\"><table><tr>%s</tr>", nm(r).c_str(), Esc(s.cls, true).c_str(),
                    L_(u8"<th>Оружие</th><th>Урон</th><th>Доля</th><th>Попадания</th><th>Убийства</th><th>Лучший удар</th><th>Пик DPS 1с / 5с</th><th>DPS в бою</th><th>Время попаданий</th>",
                       u8"<th>Weapon</th><th>Damage</th><th>Share</th><th>Hits</th><th>Kills</th><th>Best hit</th><th>Peak DPS 1s / 5s</th><th>DPS in use</th><th>Time hitting</th>"));
            for (int w = 0; w < s.nWeapon; ++w) {
                const WeaponRow &x = s.weapon[w];
                fprintf(f, "<tr><td>%s%s<div class=\"bar\"><i style=\"width:%.1f%%\"></i></div></td><td>%s</td><td>%.0f%%</td><td>%s</td><td>%u</td><td>%s</td><td>%s / %s</td><td>%s</td><td>%d:%02d</td></tr>",
                        Esc(x.name, true).c_str(), x.veh ? L_(u8" <span class=\"cls\">техника</span>", u8" <span class=\"cls\">vehicle</span>") : "",
                        tot > 0 ? 100.0 * x.dmg / tot : 0.0, Num(x.dmg).c_str(), tot > 0 ? 100.0 * x.dmg / tot : 0.0, Num(x.hits).c_str(), x.kills,
                        Num(x.best).c_str(), Num(x.peak1).c_str(), Num(x.peak5).c_str(), Num(x.active > 0 ? x.dmg / x.active : 0).c_str(),
                        (int)x.active / 60, (int)x.active % 60);
            }
            fputs("</table></div></div>\n", f);
        }
        fputs("</div>\n", f);
        fputs(L_(u8"<p class=\"note\">DPS в бою — урон оружия, делённый на время, когда оно попадало (секунды, в которые было хотя бы одно попадание). "
                 u8"Оружие различает компьютер стрелявшего; у игроков без EDFStats этого раздела нет.</p>\n",
                 u8"<p class=\"note\">DPS in use — the weapon's damage divided by the time it was hitting (seconds with at least one hit). "
                 u8"Weapons are told apart by the shooter's PC; players without EDFStats have no weapon section.</p>\n"), f);
    }

    // per player breakdown
    fputs(L_(u8"<h2>По врагам</h2><div class=\"cards\">\n", u8"<h2>By enemy</h2><div class=\"cards\">\n"), f);
    for (auto &r : rows) {
        const Summary &s = r.s;
        fprintf(f, "<div class=\"card\"><h3>%s <span class=\"cls\">%s</span></h3><table><tr>%s</tr>",
                nm(r).c_str(), Esc(s.cls, true).c_str(), L_(u8"<th>Враг</th><th>Урон</th><th>Убийства</th>", u8"<th>Enemy</th><th>Damage</th><th>Kills</th>"));
        for (int e = 0; e < s.nEnemy; ++e)
            fprintf(f, "<tr><td>%s</td><td>%s</td><td>%u</td></tr>", Esc(EnemyName(s.enemy[e].name), true).c_str(), Num(s.enemy[e].dmg).c_str(), s.enemy[e].kills);
        if (!s.nEnemy) fputs(L_(u8"<tr><td colspan=3 class=\"cls\">нет данных</td></tr>", u8"<tr><td colspan=3 class=\"cls\">no data</td></tr>"), f);
        if (s.dmgVeh > 0) fprintf(f, L_(u8"<tr><td class=\"cls\">из них с техники</td><td>%s</td><td>%u</td></tr>", u8"<tr><td class=\"cls\">of that from vehicles</td><td>%s</td><td>%u</td></tr>"), Num(s.dmgVeh).c_str(), s.killsVeh);
        if (s.ffNpc > 0 || s.ffKills) fprintf(f, L_(u8"<tr><td class=\"cls\">союзные NPC</td><td>%s</td><td>%u</td></tr>", u8"<tr><td class=\"cls\">allied NPCs</td><td>%s</td><td>%u</td></tr>"), Num(s.ffNpc).c_str(), s.ffKills);
        for (int i = 0; i < s.nFfFrom; ++i)
            fprintf(f, L_(u8"<tr><td class=\"cls\">получил от игрока-%s</td><td>%s</td><td></td></tr>", u8"<tr><td class=\"cls\">taken from the %s player</td><td>%s</td><td></td></tr>"), Esc(s.ffFrom[i].cls, true).c_str(), Num(s.ffFrom[i].dmg).c_str());
        fputs("</table></div>\n", f);
    }
    fputs("</div>\n", f);
    bool anyPartial = false; for (auto &r : rows) anyPartial |= r.partial;
    if (anyPartial)
    { fputs(L_(u8"<p class=\"note warn\">«Неполные данные» — у этого игрока не стоит EDFStats или его статистика не дошла по сети; ", u8"<p class=\"note warn\">“Partial data” — this player does not run EDFStats or their numbers did not arrive over the network; "), f);
          fputs(L_(u8"видно только то, что заметил этот компьютер (в основном добивания).</p>\n", u8"only what this PC saw is shown (mostly finishing blows).</p>\n"), f); }
    fprintf(f, L_(u8"<p class=\"note\">EDFStats %s · урон считает компьютер стрелявшего игрока, остальные получают его цифры по сети.</p>\n", u8"<p class=\"note\">EDFStats %s · damage is counted by the shooter\'s PC, the others get those numbers over the network.</p>\n"), VERSION);
    fputs("</main></body></html>\n", f);
    fclose(f);
}

static std::vector<Row> BuildRows() {             // caller holds g_cs
    std::vector<Row> rows;
    if (g_localSol) rows.push_back({ MakeSummary(P(g_localSol), g_name), true, false });
    std::vector<Summary> rem = RemoteSummaries(g_missionStartMs, GetTickCount64());
    for (auto &s : rem) rows.push_back({ s, false, false });
    // remote players without the plugin: only what this machine saw (their kill shots)
    for (auto &kv : g_ps) {
        if (kv.first == g_localSol) continue;
        std::string cn = ClassName(kv.second.cls);
        bool covered = false;
        for (auto &s : rem) covered |= cn == s.cls;
        if (!covered) rows.push_back({ MakeSummary(kv.second, ""), false, true });
    }
    // damage to other players only lowers HP on the victim's PC, so it is taken from the victims' reports
    // (matched by class; two players of the same class cannot be told apart and keep what their PC saw)
    for (auto &r : rows) {
        int same = 0; for (auto &o : rows) same += !strcmp(o.s.cls, r.s.cls);
        if (same != 1) continue;
        double got = 0; bool any = false;
        for (auto &o : rows) {
            if (&o == &r) continue;
            for (int i = 0; i < o.s.nFfFrom; ++i) if (!strcmp(o.s.ffFrom[i].cls, r.s.cls)) { got += o.s.ffFrom[i].dmg; any = true; }
        }
        if (any) r.s.ffPlayers = (float)got;
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.s.dmg > b.s.dmg; });
    return rows;
}

// what the overlay shows; refreshed by the worker thread, read by the render thread
struct LiveView { std::vector<Row> rows; double dps1 = 0, dps5 = 0, secs = 0; bool inMission = false, ended = false, autoBoard = false; std::string savedPath; };
static LiveView g_view; static CRITICAL_SECTION g_viewCs;

static void WriteResults(const char *why) {      // caller holds g_cs
    NetReceive();
    double secs = (GetTickCount64() - g_missionStartMs) / 1000.0;
    std::vector<Row> rows = BuildRows();

    Log("==== RESULTS (%s, %.0f s) ====", why, secs);
    for (auto &r : rows) {
        const Summary &s = r.s;
        Log("%-16s %-10s %s dmg=%.0f (vehicle %.0f) hits=%u kills=%u (vehicle %u) parts=%u best=%.0f peakDPS1s=%.0f peakDPS5s=%.0f "
            "toPlayers=%.0f toNPC=%.0f npcKilled=%u self=%.0f taken=%.0f takenInVehicle=%.0f takenFromAllies=%.0f deaths=%u "
            "walked=%.0fm driven=%.0fm",
            s.name, s.cls, r.local ? "[this PC]" : r.partial ? "[partial]" : "[network]", s.dmg, s.dmgVeh, s.hits, s.kills,
            s.killsVeh, s.parts, s.best, s.peak1, s.peak5, s.ffPlayers, s.ffNpc, s.ffKills, s.selfDmg, s.taken, s.takenVeh,
            s.takenFF, s.deaths, s.dist, s.distVeh);
        for (int i = 0; i < s.nFfFrom; ++i) Log("      friendly fire received from %s: %.0f", s.ffFrom[i].cls, s.ffFrom[i].dmg);
        for (int e = 0; e < s.nEnemy; ++e) Log("      %-40s dmg %8.0f  kills %u", EnemyName(s.enemy[e].name).c_str(), s.enemy[e].dmg, s.enemy[e].kills);
        for (int w = 0; w < s.nWeapon; ++w) {
            const WeaponRow &x = s.weapon[w];
            Log("      weapon %-34s dmg %8.0f  hits %u  kills %u  best %.0f  peakDPS1s %.0f  peakDPS5s %.0f  hitting %.0fs  avgDPS %.0f%s", x.name, x.dmg,
                x.hits, x.kills, x.best, x.peak1, x.peak5, x.active, x.active > 0 ? x.dmg / x.active : 0.0, x.veh ? "  (vehicle)" : "");
        }
    }
    Log("==== END ==== (packets: EOS sent %u received %u, UDP sent %u received %u)", g_txEos, g_rxEos, g_txUdp, g_rxUdp);

    char stamp[64];
    snprintf(stamp, sizeof stamp, "%04d-%02d-%02d_%02d-%02d-%02d", g_missionStart.wYear, g_missionStart.wMonth,
             g_missionStart.wDay, g_missionStart.wHour, g_missionStart.wMinute, g_missionStart.wSecond);
    std::string root = g_dir + "EDFStats\\", dir = root + "missions\\";
    CreateDirectoryA(root.c_str(), nullptr); CreateDirectoryA(dir.c_str(), nullptr);
    WriteJson(dir + stamp + ".json", rows, why, secs);
    WriteHtml(dir + stamp + ".html", rows, secs);
    CopyFileA((dir + stamp + ".html").c_str(), (root + "latest.html").c_str(), FALSE);
    Log("saved %s%s.json / .html and %slatest.html", dir.c_str(), stamp, root.c_str());
    EnterCriticalSection(&g_viewCs); g_view.savedPath = root + "latest.html"; LeaveCriticalSection(&g_viewCs);
}

#include "overlay.inc"

static void RefreshView(uint64_t now) {          // caller holds g_cs
    LiveView v;
    v.rows = BuildRows();
    if (g_localSol) { const PStats &p = P(g_localSol); v.dps1 = p.dps.Window(now, 10); v.dps5 = p.dps.Window(now, 50) / 5.0; }
    v.secs = (now - g_missionStartMs) / 1000.0;
    v.inMission = g_localSol != nullptr && !g_missionEnded;
    v.ended = g_missionEnded;
    EnterCriticalSection(&g_viewCs);
    v.autoBoard = g_autoBoard; v.savedPath = g_view.savedPath;
    g_view = std::move(v);
    LeaveCriticalSection(&g_viewCs);
}

// ---- worker thread: hotkeys, network, distance ----------------------------------------------------
static DWORD WINAPI Worker(LPVOID) {
    OverlayHook();
    bool d1 = false, d2 = false; uint64_t lastSend = 0, lastPos = 0, lastName = 0, lastView = 0;
    for (;;) {
        Sleep(50);
        uint64_t now = GetTickCount64();
        bool k1 = (GetAsyncKeyState(g_dumpKey) & 0x8000) != 0, k2 = (GetAsyncKeyState(g_resetKey) & 0x8000) != 0;
        EnterCriticalSection(&g_cs);
        NetReceive();
        if (now - lastPos >= 100) { SampleDistance(); lastPos = now; }
        if (now - lastName >= 5000) { SteamName(); lastName = now; }
        if (now - lastSend >= (uint64_t)g_sendMs) { NetSend(); lastSend = now; }
        if (k1 && !d1) { NetSend(); WriteResults("F9"); fflush(g_log); }
        if (k2 && !d2) NewMission(g_localSol);
        if (g_finalWriteMs && now >= g_finalWriteMs) {
            g_finalWriteMs = 0; WriteResults("mission end");
            g_autoBoard = g_autoBoardOnEnd;
        }
        if (now - lastView >= 250) { RefreshView(now); lastView = now; }
        LeaveCriticalSection(&g_cs);
        d1 = k1; d2 = k2;
    }
}

// ---- pattern scan + setup ---------------------------------------------------------------------------
static bool ParseSig(const char *s, std::vector<int> &out) {
    while (*s) {
        while (*s == ' ') ++s;
        if (!*s) break;
        if (s[0] == '?') { out.push_back(-1); s += (s[1] == '?') ? 2 : 1; }
        else { out.push_back((int)strtoul(std::string(s, 2).c_str(), nullptr, 16)); s += 2; }
    }
    return !out.empty();
}
static uint8_t *Scan(const char *sig, bool hooked = false) {
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
static std::string IniUtf8(const char *ini, const char *key) {
    wchar_t wini[MAX_PATH], w[256];
    MultiByteToWideChar(CP_ACP, 0, ini, -1, wini, MAX_PATH);
    GetPrivateProfileStringW(L"EDFStats", std::wstring(key, key + strlen(key)).c_str(), L"", w, 256, wini);
    char u[512]; WideCharToMultiByte(CP_UTF8, 0, w, -1, u, sizeof u, nullptr, nullptr);
    return u;
}

extern "C" __declspec(dllexport) bool EDFMLAPI EML6_Load(PluginInfo *info) {
    info->infoVersion = PluginInfo::MaxInfoVer;
    info->name = "EDFStats";
    info->version = PLUG_VER(0, 9, 1, 0);

    InitializeCriticalSection(&g_cs); InitializeCriticalSection(&g_viewCs);
    char base[MAX_PATH]; HMODULE me = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&EML6_Load, &me);
    GetModuleFileNameA(me, base, MAX_PATH);
    char *slash = strrchr(base, '\\'); g_dir = slash ? std::string(base, slash + 1) : std::string();
    char *dot = strrchr(base, '.'); if (dot) *dot = 0;
    std::string logPath = std::string(base) + ".log", iniPath = std::string(base) + ".ini";
    g_log = fopen(logPath.c_str(), "w");
    if (g_log) setvbuf(g_log, nullptr, _IOFBF, 1 << 16);
    const char *ini = iniPath.c_str();
    g_logHits = GetPrivateProfileIntA("EDFStats", "LogHits", 0, ini);
    g_wprobe = GetPrivateProfileIntA("EDFStats", "WeaponProbe", 1, ini) != 0;
    g_dumpKey = GetPrivateProfileIntA("EDFStats", "SummaryKey", VK_F9, ini);
    g_resetKey = GetPrivateProfileIntA("EDFStats", "ResetKey", VK_F10, ini);
    g_port = GetPrivateProfileIntA("EDFStats", "Port", 27787, ini);
    g_sendMs = std::max(250, (int)GetPrivateProfileIntA("EDFStats", "SendIntervalMs", 1000, ini));
    g_name = IniUtf8(ini, "Name");
    PickLanguage(IniUtf8(ini, "Language"));
    g_overlayOn = GetPrivateProfileIntA("EDFStats", "Overlay", 1, ini);
    g_uiUser = std::min(3.0f, std::max(0.5f, GetPrivateProfileIntA("EDFStats", "OverlayScale", 100, ini) / 100.0f));
    g_keyLive = GetPrivateProfileIntA("EDFStats", "LivePanelKey", VK_F6, ini);
    g_keyBoard = GetPrivateProfileIntA("EDFStats", "ScoreboardKey", VK_F7, ini);
    g_showLive = GetPrivateProfileIntA("EDFStats", "LivePanel", 1, ini) != 0;
    g_autoBoardOnEnd = GetPrivateProfileIntA("EDFStats", "ScoreboardOnResult", 1, ini) != 0;
    g_useEos = GetPrivateProfileIntA("EDFStats", "ShareViaGame", 1, ini) != 0;
    LARGE_INTEGER q; QueryPerformanceCounter(&q);
    g_self = (uint32_t)(q.QuadPart ^ GetCurrentProcessId() ^ (q.QuadPart >> 32));
    if (!g_self) g_self = 1;
    Log("EDFStats %s loaded; name='%s' LogHits=%d WeaponProbe=%d", VERSION, g_name.c_str(), g_logHits, (int)g_wprobe);

    char peers[1024];
    GetPrivateProfileStringA("EDFStats", "Peers", "", peers, sizeof peers, ini);
    if (peers[0]) ParsePeers(peers, true);
    else {   // reuse the list the player already filled in for SquadSync (its ports are ignored)
        GetPrivateProfileStringA("SquadSync", "Peers", "", peers, sizeof peers, (g_dir + "SquadSync.ini").c_str());
        if (peers[0]) { Log("Peers taken from SquadSync.ini"); ParsePeers(peers, false); }
    }
    if (g_peers.empty()) Log("WARNING: no Peers - stats are not shared, other players appear as partial");
    NetInit();
    g_nameFromIni = !g_name.empty();

    HMODULE edf = GetModuleHandleW(L"EDF.dll");
    if (!edf) { Log("EDF.dll not found"); return false; }
    g_base = (uint8_t *)edf;
    {   // .rdata range: a pointer whose first qword points there is a game object with a vtable
        auto nt = (IMAGE_NT_HEADERS *)(g_base + ((IMAGE_DOS_HEADER *)g_base)->e_lfanew);
        auto sec = IMAGE_FIRST_SECTION(nt);
        for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            if (!memcmp(sec[i].Name, ".rdata", 6)) { g_vtLo = (uintptr_t)g_base + sec[i].VirtualAddress; g_vtHi = g_vtLo + sec[i].Misc.VirtualSize; }
        if (!g_vtLo) Log("WARNING: .rdata not found, weapons are not told apart");
    }
    uint8_t *t = Scan(SIG_APPLY_DAMAGE);
    if (!t && (t = Scan(SIG_APPLY_DAMAGE, true))) Log("ApplyDamage already hooked by another plugin, chaining");
    if (!t) { Log("ERROR: ApplyDamage signature not found (different EDF.dll build?)"); return false; }
    if (t[FN_TEAMTABLE] == 0x48 && t[FN_TEAMTABLE + 1] == 0x8B && t[FN_TEAMTABLE + 2] == 0x0D)
        g_teamTable = (uint8_t **)(t + FN_TEAMTABLE + 7 + *(int32_t *)(t + FN_TEAMTABLE + 3));
    else Log("WARNING: team table instruction not found, friendly fire falls back to team ids");
    EosInit();

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) { Log("ERROR: MH_Initialize %d", st); return false; }
    if (MH_CreateHook(t, (void *)hApplyDamage, (void **)&oApplyDamage) != MH_OK || MH_EnableHook(t) != MH_OK) {
        Log("ERROR: could not hook ApplyDamage"); return false;
    }
    uint8_t *rc = Scan(SIG_RESULT_CTOR);
    if (!rc && (rc = Scan(SIG_RESULT_CTOR, true))) Log("result screen already hooked by another plugin, chaining");
    if (rc && MH_CreateHook(rc, (void *)hResultCtor, (void **)&oResultCtor) == MH_OK && MH_EnableHook(rc) == MH_OK)
        Log("mission end detection on (result screen)");
    else Log("WARNING: result screen not found, results are saved with F9 and at the next mission");
    uint8_t *cp = Scan(SIG_CREATE_PLAYER);
    if (!cp) cp = Scan(SIG_CREATE_PLAYER, true);
    if (cp && MH_CreateHook(cp, (void *)hCreatePlayer, (void **)&oCreatePlayer) == MH_OK && MH_EnableHook(cp) == MH_OK)
        Log("player spawn tracking on");
    else Log("WARNING: player spawn function not found");
    uint8_t *rd = Scan(SIG_RESULT_DTOR);
    if (!rd) rd = Scan(SIG_RESULT_DTOR, true);
    if (!rd || MH_CreateHook(rd, (void *)hResultDtor, (void **)&oResultDtor) != MH_OK || MH_EnableHook(rd) != MH_OK)
        Log("WARNING: result screen close not found, hide the scoreboard with F7");
    g_missionStartMs = GetTickCount64(); GetLocalTime(&g_missionStart);
    CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    Log("ready. F9 = write results, F10 = start counting anew. Results are saved automatically when the mission ends.");
    return true;
}
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
