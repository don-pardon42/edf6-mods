// MortarCam - pure logic without game or Windows access (tested offline in tests\test_logic.cpp).
#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace mc {

// The soldier's camera setting block ("game_object_camera_setting", 0xB0 bytes). The character camera copies it
// every frame from soldier+0x230 when the byte soldier+0x224 is 1, otherwise from soldier+0x170
// (CharacterGhostCamera update, EDF.dll+F84B5). Air Raider's bomber view writes a blended copy there
// (Engineer update, EDF.dll+563AF0).
constexpr size_t BLOCK_SIZE = 0xB0;
constexpr size_t BLK_LOOK = 0x00;     // vec4: point the camera looks at, relative to the character (Air Raider: y +35)
constexpr size_t BLK_CAM = 0x10;      // vec4: camera position relative to the character (Air Raider: y +70, z -140)
constexpr size_t BLK_ROT_ON = 0x40;   // byte: extra camera rotation on
constexpr size_t BLK_ROT = 0x50;      // float x: rotation about X (pitch, radians), +4: about Y

struct V3 { float x = 0, y = 0, z = 0; };

inline float Clamp01(float t) { return t < 0 ? 0 : (t > 1 ? 1 : t); }
// the game eases the Air Raider blend as t^3 (powf(t, 3.0) at EDF.dll+563C47)
inline float Ease(float t) { t = Clamp01(t); return t * t * t; }
// one frame of the blend value: up while on, down while off
inline float StepBlend(float b, bool on, float up, float down) { return Clamp01(on ? b + up : b - down); }

inline float RdF(const uint8_t *b, size_t o) { float f; memcpy(&f, b + o, 4); return f; }
inline void WrF(uint8_t *b, size_t o, float f) { memcpy(b + o, &f, 4); }

// out = base with LOOK/CAM xyz moved towards look/cam by Ease(t); w components stay as in base (as the game does).
// tiltRad: when non-zero, the extra pitch rotation is blended in as well.
inline void MakeBlock(const uint8_t *base, uint8_t *out, float t, V3 look, V3 cam, float tiltRad) {
    memcpy(out, base, BLOCK_SIZE);
    float e = Ease(t);
    const float lk[3] = { look.x, look.y, look.z }, cm[3] = { cam.x, cam.y, cam.z };
    for (int i = 0; i < 3; ++i) {
        float a = RdF(base, BLK_LOOK + 4 * i), c = RdF(base, BLK_CAM + 4 * i);
        WrF(out, BLK_LOOK + 4 * i, a + (lk[i] - a) * e);
        WrF(out, BLK_CAM + 4 * i, c + (cm[i] - c) * e);
    }
    if (tiltRad != 0.0f) {
        float a = base[BLK_ROT_ON] ? RdF(base, BLK_ROT) : 0.0f;
        out[BLK_ROT_ON] = 1;
        WrF(out, BLK_ROT, a + (tiltRad - a) * e);
        if (!base[BLK_ROT_ON]) WrF(out, BLK_ROT + 4, 0.0f);
    }
}

// Who owns soldier+0x230/+0x224 this frame. The game itself also uses it (Air Raider/Ranger requests, some
// HumanBase states), so the mod only writes while the block there is the one it wrote last time.
struct Owner { bool wrote = false; uint8_t last[BLOCK_SIZE] = {}; };
enum class Act { Idle, Write, Release, GameOwns };
inline Act Decide(bool flag, const uint8_t *cur, const Owner &o, float blend) {
    bool ours = o.wrote && memcmp(cur, o.last, BLOCK_SIZE) == 0;
    if (flag && !ours) return Act::GameOwns;
    if (blend > 0.0f) return Act::Write;
    if (o.wrote) return Act::Release;
    return Act::Idle;
}

// Ballistic flight like the game's own throw guide (camera EDF.dll+F7F80): velocity = dir * speed per frame,
// gravity per frame^2 = g * gravityFactor / 60 / 60. Stops when the shell comes down to groundY.
struct Shot { bool ok = false; V3 impact; int frames = 0; float dist = 0; float apex = 0; };
inline Shot Predict(V3 p, V3 dir, float speed, float gravFactor, int alive, float groundY, float g = 9.8f) {
    Shot s;
    float n = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (!(n > 1e-6f) || !(speed > 0) || alive <= 0 || alive > 100000) return s;
    V3 v{ dir.x / n * speed, dir.y / n * speed, dir.z / n * speed };
    float gy = g * gravFactor / 3600.0f;
    V3 o = p; s.apex = p.y;
    for (int f = 1; f <= alive; ++f) {
        V3 prev = p;
        p.x += v.x; p.y += v.y; p.z += v.z;
        v.y -= gy;
        if (p.y > s.apex) s.apex = p.y;
        if (v.y < 0 && p.y <= groundY) {
            float k = (prev.y - p.y) > 1e-6f ? (prev.y - groundY) / (prev.y - p.y) : 1.0f;
            p = V3{ prev.x + (p.x - prev.x) * k, groundY, prev.z + (p.z - prev.z) * k };
            s.ok = true; s.frames = f; break;
        }
        s.frames = f;
    }
    s.impact = p;
    float dx = p.x - o.x, dz = p.z - o.z;
    s.dist = std::sqrt(dx * dx + dz * dz);
    return s;
}

// ini values ------------------------------------------------------------------------------------
// "B" -> 'B'; "0x05" / "5" -> virtual-key code; anything else -> def
inline int ParseKey(const char *v, int def, int (*charToVk)(char) = nullptr) {
    if (!v || !v[0]) return def;
    if (v[1] == 0 && !(v[0] >= '0' && v[0] <= '9')) {
        char c = v[0];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if ((c >= 'A' && c <= 'Z')) return c;
        if (charToVk) { int k = charToVk(c); return k > 0 ? k : def; }
        return def;
    }
    char *end = nullptr; long x = strtol(v, &end, 0);
    if (end == v || *end) return def;
    return (x > 0 && x < 256) ? (int)x : def;
}
inline bool ParseV3(const char *s, V3 &out) {
    float a[3]; char *e;
    for (int i = 0; i < 3; ++i) {
        while (*s == ' ' || (i && *s == ',')) ++s;
        a[i] = strtof(s, &e);
        if (e == s) return false;
        s = e;
    }
    while (*s == ' ') ++s;
    if (*s) return false;
    out = V3{ a[0], a[1], a[2] };
    return true;
}
// case-insensitive (ASCII) substring match against a comma separated list; UTF-8 parts compare byte-exact
inline bool NameMatches(const std::string &name, const std::string &list) {
    auto low = [](std::string s) { for (auto &c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; };
    std::string n = low(name);
    size_t i = 0;
    while (i <= list.size()) {
        size_t j = list.find(',', i); if (j == std::string::npos) j = list.size();
        std::string part = list.substr(i, j - i);
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        while (!part.empty() && part.back() == ' ') part.pop_back();
        if (!part.empty() && n.find(low(part)) != std::string::npos) return true;
        i = j + 1;
    }
    return false;
}

} // namespace mc
