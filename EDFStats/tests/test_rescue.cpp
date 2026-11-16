// rescue detection: fake player objects, fake CreateOnlinePlayerObject
#include "edfstats.cpp"

static uint8_t *g_next;
static uint8_t *__fastcall FakeCreate(unsigned, const float *, int) { return g_next; }
static uint8_t *Obj(float x, float y, float z) {
    uint8_t *o = (uint8_t *)calloc(1, 0x2000);
    float p[3] = { x, y, z }; memcpy(o + SB_POS, p, sizeof p);
    return o;
}
static void Spawn(unsigned slot, uint8_t *o) {
    float m[16] = {}; memcpy(m + 12, o + SB_POS, 12);
    g_next = o; hCreatePlayer(slot, m, 0);
}
int main() {
    InitializeCriticalSection(&g_cs); InitializeCriticalSection(&g_viewCs);
    g_log = stdout; oCreatePlayer = FakeCreate;
    uint8_t *A = Obj(0, 0, 0), *B = Obj(100, 0, 0), *C = Obj(300, 0, 0);
    g_lastFightMs = GetTickCount64();
    Spawn(0, A); Spawn(1, B); Spawn(2, C);
    g_localSol = A; g_missionStartMs = GetTickCount64(); P(A).cls = "Engineer";
    g_lastFightMs = GetTickCount64();

    // 1) B goes down next to A (5 m), comes back -> A rescued B
    { float bp[3] = { 5, 0, 0 }; memcpy(B + SB_POS, bp, 12); } B[GO_DEAD] = 1; g_downMs[B] = GetTickCount64() - 8000;
    uint8_t *B2 = Obj(5, 0, 0); Spawn(1, B2);
    printf("1) A.revives=%u (expect 1)\n", P(A).revives);

    // 2) C respawns far from everyone without being down -> nothing
    uint8_t *C2 = Obj(300, 0, 0); Spawn(2, C2);
    printf("2) A.revives=%u (expect 1)\n", P(A).revives);

    // 3) A (this PC) goes down, B2 rescues it -> revived=1, down time ~12 s, stats move to the new object
    P(A).dmg = 1234; A[GO_DEAD] = 1; g_downMs[A] = GetTickCount64() - 12000;
    uint8_t *A2 = Obj(1, 0, 0); Spawn(0, A2);
    PStats &me = P(g_localSol);
    printf("3) local is new object=%d revived=%u downSecs=%.0f dmg kept=%.0f revives kept=%u (expect 1 1 12 1234 1)\n",
           g_localSol == A2, me.revived, me.downSecs, me.dmg, me.revives);

    // 4) 2 minutes without any fight, then a spawn: new mission, not a rescue
    g_lastFightMs = GetTickCount64() - 120000;
    A2[GO_DEAD] = 1; uint8_t *A3 = Obj(0, 0, 0); unsigned before = P(g_localSol).revived; Spawn(0, A3);
    printf("4) revived unchanged=%d (expect 1)\n", P(A2).revived == before);
    return 0;
}
