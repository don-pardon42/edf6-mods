// Offline tests for MortarCam's pure logic (no game needed).
#include <cstdio>
#include <cmath>
#include "mortarcam_logic.h"

static int g_fail = 0, g_n = 0;
#define CHECK(c) do { ++g_n; if (!(c)) { ++g_fail; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
static bool Near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static void MakeBase(uint8_t *b) {
    for (size_t i = 0; i < mc::BLOCK_SIZE; ++i) b[i] = (uint8_t)(0xA0 + i);   // recognisable filler
    float look[4] = { 0, 1.5f, 0, 1 }, cam[4] = { 0.5f, 2, -4, 1 };
    memcpy(b + mc::BLK_LOOK, look, 16); memcpy(b + mc::BLK_CAM, cam, 16);
    b[mc::BLK_ROT_ON] = 0;
}

static void TestBlend() {
    CHECK(Near(mc::Ease(0), 0)); CHECK(Near(mc::Ease(1), 1)); CHECK(Near(mc::Ease(0.5f), 0.125f));
    CHECK(Near(mc::Ease(2), 1)); CHECK(Near(mc::Ease(-1), 0));
    float b = 0; for (int i = 0; i < 100; ++i) b = mc::StepBlend(b, true, 0.05f, 0.08f);
    CHECK(Near(b, 1));
    b = mc::StepBlend(b, false, 0.05f, 0.08f); CHECK(Near(b, 0.92f));
    for (int i = 0; i < 100; ++i) b = mc::StepBlend(b, false, 0.05f, 0.08f);
    CHECK(b == 0.0f);
}

static void TestMakeBlock() {
    uint8_t base[mc::BLOCK_SIZE], out[mc::BLOCK_SIZE];
    MakeBase(base);
    mc::V3 look{ 0, 35, 0 }, cam{ 0, 70, -140 };
    mc::MakeBlock(base, out, 0, look, cam, 0);
    CHECK(memcmp(base, out, mc::BLOCK_SIZE) == 0);                  // t = 0: unchanged
    mc::MakeBlock(base, out, 1, look, cam, 0);
    CHECK(Near(mc::RdF(out, 0x00), 0)); CHECK(Near(mc::RdF(out, 0x04), 35)); CHECK(Near(mc::RdF(out, 0x08), 0));
    CHECK(Near(mc::RdF(out, 0x0C), 1));                             // w kept
    CHECK(Near(mc::RdF(out, 0x10), 0)); CHECK(Near(mc::RdF(out, 0x14), 70)); CHECK(Near(mc::RdF(out, 0x18), -140));
    CHECK(Near(mc::RdF(out, 0x1C), 1));
    CHECK(memcmp(base + 0x20, out + 0x20, mc::BLOCK_SIZE - 0x20) == 0);   // everything after the two vectors untouched
    mc::MakeBlock(base, out, 0.5f, look, cam, 0);                   // eased: 0.125 of the way
    CHECK(Near(mc::RdF(out, 0x04), 1.5f + (35 - 1.5f) * 0.125f));
    CHECK(Near(mc::RdF(out, 0x10), 0.5f + (0 - 0.5f) * 0.125f));
    // tilt
    float tilt = -0.5f;
    mc::MakeBlock(base, out, 1, look, cam, tilt);
    CHECK(out[mc::BLK_ROT_ON] == 1); CHECK(Near(mc::RdF(out, mc::BLK_ROT), tilt)); CHECK(Near(mc::RdF(out, mc::BLK_ROT + 4), 0));
    mc::MakeBlock(base, out, 0, look, cam, tilt);
    CHECK(Near(mc::RdF(out, mc::BLK_ROT), 0));
    base[mc::BLK_ROT_ON] = 1; mc::WrF(base, mc::BLK_ROT, 0.2f); mc::WrF(base, mc::BLK_ROT + 4, 0.7f);
    mc::MakeBlock(base, out, 1, look, cam, tilt);
    CHECK(Near(mc::RdF(out, mc::BLK_ROT), tilt)); CHECK(Near(mc::RdF(out, mc::BLK_ROT + 4), 0.7f));   // the game's own yaw kept
}

static void TestOwner() {
    uint8_t mine[mc::BLOCK_SIZE], other[mc::BLOCK_SIZE];
    MakeBase(mine); MakeBase(other); other[5] ^= 1;
    mc::Owner o;
    CHECK(mc::Decide(false, other, o, 0) == mc::Act::Idle);
    CHECK(mc::Decide(false, other, o, 0.1f) == mc::Act::Write);
    CHECK(mc::Decide(true, other, o, 0.1f) == mc::Act::GameOwns);     // the game set the flag itself
    o.wrote = true; memcpy(o.last, mine, sizeof mine);
    CHECK(mc::Decide(true, mine, o, 0.5f) == mc::Act::Write);         // our block is still there
    CHECK(mc::Decide(true, mine, o, 0) == mc::Act::Release);          // blend back at 0: give the camera back
    CHECK(mc::Decide(true, other, o, 0.5f) == mc::Act::GameOwns);     // the game replaced our block
    CHECK(mc::Decide(false, mine, o, 0) == mc::Act::Release);         // flag cleared by the game: just forget
    o.wrote = false;
    CHECK(mc::Decide(false, mine, o, 0) == mc::Act::Idle);
}

static void TestPredict() {
    // 45 degrees, 1 m/frame = 60 m/s, g = 9.8: range on flat ground v^2/g = 367.3 m, flight 2*v*sin45/g = 8.66 s
    mc::Shot s = mc::Predict({ 0, 0, 0 }, { 0, 1, 1 }, 1.0f, 1.0f, 2000, 0);
    CHECK(s.ok);
    CHECK(std::fabs(s.dist - 367.3f) < 367.3f * 0.02f);
    CHECK(std::fabs(s.frames / 60.0f - 8.66f) < 0.1f);
    CHECK(std::fabs(s.apex - 91.8f) < 2.0f);
    CHECK(Near(s.impact.y, 0, 1e-3f)); CHECK(Near(s.impact.x, 0, 1e-3f));
    // gravity factor 2 -> half the range
    mc::Shot s2 = mc::Predict({ 0, 0, 0 }, { 0, 1, 1 }, 1.0f, 2.0f, 2000, 0);
    CHECK(s2.ok && std::fabs(s2.dist - s.dist / 2) < 4.0f);
    // fired from 2 m above the ground: lands a little further
    mc::Shot s3 = mc::Predict({ 10, 2, 0 }, { 0, 1, 1 }, 1.0f, 1.0f, 2000, 0);
    CHECK(s3.ok && s3.dist > s.dist && s3.dist < s.dist + 5);
    CHECK(Near(s3.impact.x, 10, 1e-3f));
    // short life: no landing
    mc::Shot s4 = mc::Predict({ 0, 0, 0 }, { 0, 1, 1 }, 1.0f, 1.0f, 60, 0);
    CHECK(!s4.ok && s4.frames == 60);
    // no gravity, flying up: never lands
    CHECK(!mc::Predict({ 0, 0, 0 }, { 0, 1, 0 }, 1.0f, 0.0f, 600, 0).ok);
    // bad input
    CHECK(!mc::Predict({ 0, 0, 0 }, { 0, 0, 0 }, 1.0f, 1.0f, 600, 0).ok);
    CHECK(!mc::Predict({ 0, 0, 0 }, { 0, 1, 1 }, 0.0f, 1.0f, 600, 0).ok);
    CHECK(!mc::Predict({ 0, 0, 0 }, { 0, 1, 1 }, NAN, 1.0f, 600, 0).ok);
}

static int FakeVk(char c) { return c == '=' ? 0xBB : 0; }
static void TestIni() {
    CHECK(mc::ParseKey("B", 1) == 'B'); CHECK(mc::ParseKey("b", 1) == 'B');
    CHECK(mc::ParseKey("0x05", 1) == 5); CHECK(mc::ParseKey("187", 1) == 187);
    CHECK(mc::ParseKey("", 7) == 7); CHECK(mc::ParseKey("0x100", 7) == 7); CHECK(mc::ParseKey("zz", 7) == 7);
    CHECK(mc::ParseKey("=", 7, FakeVk) == 0xBB); CHECK(mc::ParseKey("=", 7) == 7);
    CHECK(mc::ParseKey("5", 7) == 5);
    mc::V3 v;
    CHECK(mc::ParseV3("0,35,0", v) && Near(v.y, 35));
    CHECK(mc::ParseV3(" 1.5 , -2, 3e1 ", v) && Near(v.x, 1.5f) && Near(v.y, -2) && Near(v.z, 30));
    CHECK(!mc::ParseV3("1,2", v)); CHECK(!mc::ParseV3("1,2,3,4", v)); CHECK(!mc::ParseV3("a,b,c", v));
    CHECK(mc::NameMatches("YH2 Dispersal Mortar", "Mortar"));
    CHECK(mc::NameMatches("YH2 Dispersal Mortar", "cannon, mortar"));
    CHECK(!mc::NameMatches("Flame Revolver", "Mortar"));
    CHECK(!mc::NameMatches("anything", ""));
    CHECK(!mc::NameMatches("anything", " , ,"));
    CHECK(mc::NameMatches(u8"Миномёт YH2", u8"Гаубица,Миномёт"));
}

int main() {
    TestBlend(); TestMakeBlock(); TestOwner(); TestPredict(); TestIni();
    printf("%d checks, %d failed\n", g_n, g_fail);
    return g_fail ? 1 : 0;
}
