// rescue detection: fake player objects. In the game a rescued player comes back on the same
// object (2026-11-18 test), so rescues are found by polling the down state of every player slot.
#include "edfstats.cpp"

static uint8_t *__fastcall FakeCreate(unsigned, const float *, int) { return nullptr; }
static uint8_t *Obj(float x, float y, float z, float hp = 100) {
    uint8_t *o = (uint8_t *)calloc(1, 0x2000);
    float p[3] = { x, y, z }; memcpy(o + SB_POS, p, sizeof p); memcpy(o + GO_HP, &hp, 4);
    return o;
}
static void Move(uint8_t *o, float x) { float p[3] = { x, 0, 0 }; memcpy(o + SB_POS, p, 12); }
static void Down(uint8_t *o, uint64_t agoMs) { o[GO_DEAD] = 1; float z = 0; memcpy(o + GO_HP, &z, 4); PollRescues(); g_downMs[o] = GetTickCount64() - agoMs; }
static void Up(uint8_t *o, float hp = 40) { o[GO_DEAD] = 0; memcpy(o + GO_HP, &hp, 4); PollRescues(); }
int main() {
    InitializeCriticalSection(&g_cs); InitializeCriticalSection(&g_viewCs);
    g_log = stdout; oCreatePlayer = FakeCreate;
    uint8_t *A = Obj(0, 0, 0), *B = Obj(100, 0, 0), *C = Obj(300, 0, 0);
    g_slots[0].obj = A; g_slots[1].obj = B; g_slots[2].obj = C;
    g_localSol = A; g_missionStartMs = GetTickCount64(); P(A).cls = "Engineer";
    PollRescues();

    // 1) B goes down next to A (5 m) and gets back up -> A rescued B
    Move(B, 5); Down(B, 8000); Up(B);
    printf("1) A.revives=%u (expect 1)\n", P(A).revives);

    // 2) C goes down far from everyone and gets up (nobody close) -> no credit
    Down(C, 3000); Up(C);
    printf("2) A.revives=%u (expect 1)\n", P(A).revives);

    // 3) A (this PC) goes down, B rescues it -> revived=1, down time ~12 s, same object keeps its stats
    P(A).dmg = 1234; Move(B, 1); Down(A, 12000); Up(A);
    PStats &me = P(g_localSol);
    printf("3) same object=%d revived=%u downSecs=%.0f dmg kept=%.0f revives kept=%u (expect 1 1 12 1234 1)\n",
           g_localSol == A, me.revived, me.downSecs, me.dmg, me.revives);

    // 4) HP only (dead flag never set): down at HP 0, up at HP > 0 -> counted
    Move(B, 3); { float z = 0; memcpy(B + GO_HP, &z, 4); } PollRescues(); g_downMs[B] = GetTickCount64() - 2000;
    { float h = 30; memcpy(B + GO_HP, &h, 4); } PollRescues();
    printf("4) A.revives=%u (expect 2)\n", P(A).revives);

    // 5) after the result screen nothing is counted
    g_missionEnded = true; Down(B, 1000); Up(B);
    printf("5) A.revives=%u (expect 2)\n", P(A).revives);
    return 0;
}
