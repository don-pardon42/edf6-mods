// weapon detection: fake game objects with fake vtables; the bullet / weapon is kept in the caller's
// stack frame like the game's bullet code does, then the hit goes through the real hook.
#include "edfstats.cpp"

static uintptr_t fakeRdata[64];                    // fake vtables live here
static uintptr_t VT(int i) { return (uintptr_t)&fakeRdata[i * 4]; }
static uint8_t *Obj(int vt, size_t size = 0x2000) { uint8_t *o = (uint8_t *)calloc(1, size); *(uintptr_t *)o = VT(vt); return o; }
static void *__fastcall FakeApply(uint8_t *self, uint8_t *info) {
    float hp = Rd<float>(self + GO_HP) - Rd<float>(info + DI_DAMAGE);
    if (hp <= 0) { hp = 0; self[GO_DEAD] = 1; }
    memcpy(self + GO_HP, &hp, 4); return nullptr;
}
static uint8_t *me, *w1, *w2, *w3, *b1, *b0;   // globals: not on main's stack, like the game's heap objects
static uint8_t *Enemy(float hp) { uint8_t *e = Obj(3); memcpy(e + GO_HP, &hp, 4); int32_t t = 1; memcpy(e + GO_TEAM, &t, 4); return e; }
static uint8_t *Info(float dmg) {
    uint8_t *i = (uint8_t *)calloc(1, 0x100); *(uint8_t **)(i + DI_ATTACKER) = me; memcpy(i + DI_DAMAGE, &dmg, 4);
    return i;
}
// the "bullet code": keeps its objects in its own frame while the damage is applied
__declspec(noinline) static void HitFrom(uint8_t *a, uint8_t *b, uint8_t *target, float dmg) {
    volatile uintptr_t keep[4] = { 0x1234, (uintptr_t)a, (uintptr_t)b, 0 };
    uint8_t *info = Info(dmg);
    hApplyDamage(target, info);
    keep[3] = keep[0];
}
static void Hit(uint8_t *a, uint8_t *b, float dmg, float enemyHp = 1000) { HitFrom(a, b, Enemy(enemyHp), dmg); }
static const WStats *W(uint8_t *w) { auto &m = P(me).weapons; auto it = m.find(w); return it == m.end() ? nullptr : &it->second; }

int main(int argc, char **argv) {
    InitializeCriticalSection(&g_cs); InitializeCriticalSection(&g_viewCs);
    g_log = stdout; g_dir = argc > 1 ? argv[1] : ".\\"; if (argc > 2 && !strcmp(argv[2], "en")) g_en = true;
    oApplyDamage = FakeApply;
    g_vtLo = (uintptr_t)fakeRdata; g_vtHi = (uintptr_t)(fakeRdata + 64); g_base = (uint8_t *)1;
    g_cls[VT(0)] = { "Weapon_BasicShoot", K_WEAPON }; g_cls[VT(1)] = { "SolidBullet01", K_BULLET };
    g_cls[VT(2)] = { "Engineer", K_SOLDIER };          g_cls[VT(3)] = { "EDF6_GiantAnt", K_OTHER };
    g_cls[VT(4)] = { "Weapon_HeavyShoot", K_WEAPON };
    me = Obj(2); *(void **)(me + SB_NETPLAYER) = me; *(void **)(me + SB_CTRL) = me;   // local player

    // weapon 1 carries its name (pointer to a UTF-16 string), weapon 2 and 3 have none
    w1 = Obj(0); w2 = Obj(4); w3 = Obj(4);
    static const wchar_t name1[] = L"Гранатомёт UM-4A";
    *(const wchar_t **)(w1 + 0x48) = name1;
    *(const wchar_t **)(w1 + 0x60) = L"app:/weapon/G_UM4A.sgo";   // a path is not taken as the name
    b1 = Obj(1); *(uint8_t **)(b1 + 0x2A0) = w1;     // bullet -> its weapon
    b0 = Obj(1);                                     // a bullet without a weapon link (e.g. a support unit's)

    EnterCriticalSection(&g_cs); NewMission(me); LeaveCriticalSection(&g_cs);
    // 1) bullet of weapon 1 on the stack, an unrelated weapon further up: the closer bullet wins
    Hit(b1, w2, 100); Hit(b0, b1, 50);
    printf("1) w1 dmg=%.0f hits=%u name=%s (expect 150 2 %s)\n", W(w1) ? W(w1)->dmg : -1, W(w1) ? W(w1)->hits : 0,
           WeaponName(w1).c_str(), Utf8(name1, (int)wcslen(name1)).c_str());
    // 2) Fencer: two weapons firing together, each hit sees only its own weapon
    for (int i = 0; i < 10; ++i) { Hit(w2, nullptr, 30); Hit(w3, nullptr, 70); }
    printf("2) w2 dmg=%.0f w3 dmg=%.0f names '%s' / '%s' (expect 300 700, two different names)\n",
           W(w2) ? W(w2)->dmg : -1, W(w3) ? W(w3)->dmg : -1, WeaponName(w2).c_str(), WeaponName(w3).c_str());
    // 3) a kill is credited to the weapon
    Hit(b1, nullptr, 500, 200);
    printf("3) w1 kills=%u best=%.0f (expect 1 200)\n", W(w1) ? W(w1)->kills : 99, W(w1) ? W(w1)->best : -1);
    // 4) only a bullet without a weapon link -> 'not recognised'
    Hit(b0, nullptr, 40);
    printf("4) unknown dmg=%.0f (expect 40)\n", W(nullptr) ? W(nullptr)->dmg : -1);
    // 5) totals agree, summary rows, report
    double sum = 0; for (auto &kv : P(me).weapons) sum += kv.second.dmg;
    EnterCriticalSection(&g_cs);
    Summary s = MakeSummary(P(me), "Tester");
    printf("5) player dmg=%.0f sum over weapons=%.0f rows=%u first='%s' %.0f (expect equal, 4 rows, the 700 weapon first)\n",
           P(me).dmg, sum, s.nWeapon, s.weapon[0].name, s.weapon[0].dmg);
    printf("   packet %zu bytes (EOS limit 1170)\n", sizeof(Packet));
    WriteResults("test");
    LeaveCriticalSection(&g_cs);
    return 0;
}
