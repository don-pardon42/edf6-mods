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
static uint8_t * volatile b1; static uint8_t * volatile b0;   // volatile: main must not keep them in a saved register
static uint8_t *me, *w1, *w2, *w3, *fw[4], *fac[4], *dev; static uintptr_t g_infoExtra;   // globals: not on main's stack, like the game's heap objects
static uint8_t *Enemy(float hp) { uint8_t *e = Obj(3); memcpy(e + GO_HP, &hp, 4); int32_t t = 1; memcpy(e + GO_TEAM, &t, 4); return e; }
static uint8_t *Info(float dmg) {
    uint8_t *i = (uint8_t *)calloc(1, 0x100); *(uint8_t **)(i + DI_ATTACKER) = me; memcpy(i + DI_DAMAGE, &dmg, 4); *(uintptr_t *)(i + 0xB0) = g_infoExtra;
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
// wipes what earlier calls left on the stack, so a test does not see an older bullet
__declspec(noinline) static void ScrubStack() { volatile char z[0x1000]; for (auto &c : z) c = 0; }
static const WStats *W(uint8_t *w) { auto &m = P(me).weapons; auto it = m.find(w); return it == m.end() ? nullptr : &it->second; }

int main(int argc, char **argv) {
    InitializeCriticalSection(&g_cs); InitializeCriticalSection(&g_viewCs);
    g_log = stdout; g_dir = argc > 1 ? argv[1] : ".\\"; if (argc > 2 && !strcmp(argv[2], "en")) g_en = true;
    oApplyDamage = FakeApply;
    g_vtLo = (uintptr_t)fakeRdata; g_vtHi = (uintptr_t)(fakeRdata + 64); g_base = (uint8_t *)1;
    g_cls[VT(0)] = { "Weapon_BasicShoot", K_WEAPON }; g_cls[VT(1)] = { "SolidBullet01", K_BULLET };
    g_cls[VT(2)] = { "Engineer", K_SOLDIER };          g_cls[VT(3)] = { "EDF6_GiantAnt", K_OTHER };
    g_cls[VT(4)] = { "Weapon_HeavyShoot", K_WEAPON }; g_cls[VT(5)] = { "Factory@SolidBullet01", K_OTHER };
    me = Obj(2); *(void **)(me + SB_NETPLAYER) = me; *(void **)(me + SB_CTRL) = me;   // local player

    // weapon 1 carries its name (pointer to a UTF-16 string), weapon 2 and 3 have none
    w1 = Obj(0); w2 = Obj(4); w3 = Obj(4);
    static const wchar_t name1[] = L"Гранатомёт UM-4A";
    *(const wchar_t **)(w1 + 0x48) = name1;
    *(const wchar_t **)(w1 + 0x60) = L"app:/weapon/G_UM4A.sgo";   // a path is not taken as the name
    b1 = Obj(1); *(uint8_t **)(b1 + 0x2A0) = w1;     // bullet -> its weapon
    b0 = Obj(1);                                     // a bullet without a weapon link (e.g. a support unit's)

    EnterCriticalSection(&g_cs); NewMission(me); LeaveCriticalSection(&g_cs);
    // 4) only a bullet without a weapon link -> 'not recognised' (first: no other bullet was on the stack yet)
    ScrubStack(); Hit(b0, nullptr, 40);
    printf("4) unknown dmg=%.0f (expect 40)\n", W(nullptr) ? W(nullptr)->dmg : -1);
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
    // Fencer-like setup: four weapons in an array the soldier points to, each with its own bullet factory
    // (+0x7F8) and its name at +0x1B0; all share a device pointer (+0x2E0), which tells nothing
    {   uint8_t **arr = (uint8_t **)calloc(8, 8); *(uint8_t ***)(me + 0x1950) = arr; dev = (uint8_t *)calloc(1, 64);
        static const wchar_t *nm[4] = { L"Пушка Арбалет", L"Щит ДВ", L"Гатлинг ДМ", L"Ракеты Сокол" };
        for (int i = 0; i < 4; ++i) {
            fw[i] = Obj(4); fac[i] = Obj(5); arr[i] = fw[i];
            *(uint8_t **)(fw[i] + 0x7F8) = fac[i]; *(uint8_t **)(fw[i] + 0x2E0) = dev; *(const wchar_t **)(fw[i] + 0x1B0) = nm[i];
        } }
    g_widx.clear();                                  // the game rebuilds the list every 3 s; here at once
    // 6) a bullet object that points only to its weapon's factory (and the shared device)
    { uint8_t *bf = Obj(1); *(uint8_t **)(bf + 0x48) = dev; *(uint8_t **)(bf + 0x40) = fac[1]; Hit(bf, nullptr, 11); }
    // 7) a BulletMtSystem record (plain memory, no vtable) that points to the factory of weapon 3
    { uint8_t *rec = (uint8_t *)calloc(1, 0x200); *(uint8_t **)(rec + 0x18) = dev; *(uint8_t **)(rec + 0x110) = fac[2]; *(uint8_t **)(rec + 0x20) = me; Hit(rec, nullptr, 13); }
    // 8) a record with a pointer into weapon 4 (a sub-object)
    { uint8_t *rec = (uint8_t *)calloc(1, 0x200); *(uint8_t **)(rec + 0x30) = fw[3] + 0x1650; *(uint8_t **)(rec + 0x8) = me; Hit(rec, nullptr, 17); }
    // 9) the damage info refers to weapon 1's factory
    g_infoExtra = (uintptr_t)fac[0]; Hit(nullptr, nullptr, 19); g_infoExtra = 0;
    // 10) two vehicle weapons 0x1710 apart (like Fuel / Autocannon (Right)): a pointer just inside the upper one
    {   uint8_t *block = (uint8_t *)calloc(1, 0x4000);
        uint8_t *lo = block, *hi = block + 0x1710; *(uintptr_t *)lo = VT(4); *(uintptr_t *)hi = VT(4);
        static const wchar_t *n2[2] = { L"Fuel", L"Autocannon (Right)" };
        *(const wchar_t **)(lo + 0x1B0) = n2[0]; *(const wchar_t **)(hi + 0x1B0) = n2[1];
        uint8_t **arr = *(uint8_t ***)(me + 0x1950); arr[4] = lo; arr[5] = hi; g_widx.clear();
        uint8_t *rec = (uint8_t *)calloc(1, 0x200); *(uint8_t **)(rec + 0x8) = me; *(uint8_t **)(rec + 0x40) = hi + 0x20;
        Hit(rec, nullptr, 23);
        printf("10) %s=%.0f %s=%.0f (expect 0 23)\n", WeaponName(lo).c_str(), W(lo) ? W(lo)->dmg : 0, WeaponName(hi).c_str(), W(hi) ? W(hi)->dmg : 0); }
    // 11b) an NPC's weapon referenced from the soldier is not ours (its owner field names another soldier)
    {   uint8_t *npc = Obj(2); uint8_t *sn = Obj(0); *(uint8_t **)(sn + 0x38) = npc; (*(uint8_t ***)(me + 0x1950))[7] = sn; g_widx.clear();
        WIdx &x = WeaponsOf(me, GetTickCount64());
        printf("11b) NPC weapon listed=%d, own weapons still listed=%d (expect 0 1)\n", (int)(std::find(x.list.begin(), x.list.end(), sn) != x.list.end()),
               (int)(std::find(x.list.begin(), x.list.end(), fw[0]) != x.list.end()));
        (*(uint8_t ***)(me + 0x1950))[7] = nullptr; g_widx.clear(); }
    // 11) an accessory is never one of the shooter's weapons
    {   g_cls[VT(6)] = { "Weapon_Accessory", K_WEAPON }; uint8_t *acc = Obj(6); (*(uint8_t ***)(me + 0x1950))[6] = acc; g_widx.clear();
        WIdx &x = WeaponsOf(me, GetTickCount64());
        printf("11) accessory listed=%d (expect 0)\n", (int)(std::find(x.list.begin(), x.list.end(), acc) != x.list.end())); }
    printf("6-9) %s=%.0f %s=%.0f %s=%.0f %s=%.0f (expect 19 11 13 17)\n", WeaponName(fw[0]).c_str(), W(fw[0]) ? W(fw[0])->dmg : -1,
           WeaponName(fw[1]).c_str(), W(fw[1]) ? W(fw[1])->dmg : -1, WeaponName(fw[2]).c_str(), W(fw[2]) ? W(fw[2])->dmg : -1,
           WeaponName(fw[3]).c_str(), W(fw[3]) ? W(fw[3])->dmg : -1);

    // 5) totals agree, summary rows, report
    double sum = 0; for (auto &kv : P(me).weapons) sum += kv.second.dmg;
    EnterCriticalSection(&g_cs);
    Summary s = MakeSummary(P(me), "Tester");
    printf("5) player dmg=%.0f sum over weapons=%.0f rows=%u first='%s' %.0f (expect equal, 6 rows incl. 'other', the 700 weapon first)\n",
           P(me).dmg, sum, s.nWeapon, s.weapon[0].name, s.weapon[0].dmg);
    printf("   packet %zu bytes (EOS limit 1170)\n", sizeof(Packet));
    WriteResults("test");
    // 12) shield probe: a Fencer's shield loses durability -> the changed number is logged
    {   g_cls[VT(7)] = { "HeavyArmor", K_SOLDIER }; g_cls[VT(8)] = { "Weapon_Shield", K_WEAPON };
        uint8_t *fen = Obj(7); uint8_t *sh = Obj(8); uint8_t **arr = (uint8_t **)calloc(4, 8); arr[0] = sh; *(uint8_t ***)(fen + 0x1950) = arr;
        float d = 1500; memcpy(sh + 0x6A0, &d, 4);
        g_localSol = fen; ProbeShields(GetTickCount64());
        d = 1320; memcpy(sh + 0x6A0, &d, 4); ProbeShields(GetTickCount64() + 100);
        for (int k = 1; k <= 30; ++k) { float px = (float)k; memcpy(sh + 0x90, &px, 4); ProbeShields(GetTickCount64() + 200 + k); }   // a moving position field
        g_missionEnded = false; ShieldSummary();
        printf("12) summary above lists +06A0 (first 1500 last 1320, down 1) before +0090 (up 29) (expect so)\n");
        // 13) shield durability (+0xBE8): 873 -> 800 (block) -> 900 (refill, not counted) -> 850 (block)
        static const wchar_t shn[] = L"Tower Shield"; *(const wchar_t **)(sh + 0x1B0) = shn; g_weapons.clear();
        auto setDur = [&](int32_t v) { memcpy(sh + 0xBE8, &v, 4); TrackShields(GetTickCount64()); };
        setDur(873); setDur(800); setDur(900); setDur(850);
        Summary fs = MakeSummary(P(fen), "Fencer");
        printf("13) shield spent=%.0f blocks=%u name=%s (expect 123 2 Tower Shield)\n", fs.shieldSpent, fs.shieldBlocks, fs.shieldName);
        printf("    packet %zu bytes (EOS limit 1170)\n", sizeof(Packet)); }
    LeaveCriticalSection(&g_cs);
    return 0;
}
