// offline harness for EDFStats: fake stats, real UDP on localhost, and a fake EOS P2P layer
#include "edfstats.cpp"
#include <deque>

// ---- fake EOS: a queue of incoming packets and a record of what was sent / accepted ----
struct FakePkt { EOS_PUID peer; std::string sock; std::vector<uint8_t> data; };
static std::deque<FakePkt> q_in; static int sent = 0, accepted = 0; static std::string lastSentSock; static uint8_t lastSentCh;
static int32_t FakeRecv(EOS_HP2P, const EOS_RecvOpts *, EOS_PUID *peer, EOS_SocketId *sock, uint8_t *ch, void *data, uint32_t *bytes) {
    if (q_in.empty()) return 13;   // EOS_NotFound
    FakePkt p = q_in.front(); q_in.pop_front();
    *peer = p.peer; strcpy(sock->SocketName, p.sock.c_str()); *ch = 0;
    memcpy(data, p.data.data(), p.data.size()); *bytes = (uint32_t)p.data.size();
    { std::vector<Row> rows; }
    return 0;
}
static int32_t FakeSend(EOS_HP2P, const EOS_SendOpts *o) { ++sent; lastSentSock = o->SocketId->SocketName; lastSentCh = o->Channel; return 0; }
static int32_t FakeAccept(EOS_HP2P, const EOS_AcceptOpts *o) { ++accepted; return 0; }

int main(int argc, char **argv) {
    InitializeCriticalSection(&g_cs); InitializeCriticalSection(&g_eosCs); InitializeCriticalSection(&g_viewCs);
    g_dir = argv[1]; g_log = stdout; g_name = u8"Игрок 1"; g_self = 0x1111;
    g_port = 27901; ParsePeers("127.0.0.1", false); NetInit();
    g_missionStartMs = GetTickCount64() - 400000; GetLocalTime(&g_missionStart);
    static uint8_t fakeLocal[0x2000] = {}, fakeFencer[0x2000] = {};
    g_localSol = fakeLocal;
    PStats &p = g_ps[fakeLocal]; p.cls = "Engineer";
    for (int i = 0; i < 300; ++i) { double d = 60 + i % 7 * 10; p.dmg += d; ++p.hits; p.dps.Add(100000 + i * 40, d); p.best = std::max(p.best, d); p.dmgBy[u8"Берсеркер"] += d; }
    p.kills = 20; p.killsBy[u8"Берсеркер"] = 20; p.parts = 2; p.dmgBy[u8"Ракетный кулак берсеркера"] = 123;
    p.ffNpc = 268; p.selfDmg = 40; p.takenFF = 150; p.ffFrom["Fencer"] = 150; p.taken = 171; p.deaths = 1; p.dist = 845.3; p.distVeh = 1210; p.dmgVeh = 9000; p.killsVeh = 6;
    PStats &f = g_ps[fakeFencer]; f.cls = "HeavyArmor"; f.kills = 12; f.killsBy[u8"Берсеркер"] = 9;   // partial view of remote

    // remote Fencer (has the plugin) over UDP
    SOCKET o = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(27901); inet_pton(AF_INET, "127.0.0.1", &to.sin_addr);
    PStats r; r.cls = "HeavyArmor"; r.dmg = 25000; r.hits = 900; r.kills = 31; r.killsBy[u8"Берсеркер"] = 31; r.dmgBy[u8"Берсеркер"] = 25000;
    r.taken = 900; r.ffFrom["Air Raider"] = 80; r.takenFF = 80; r.dist = 1500; r.dps.Add(5000, 700); r.best = 120;
    Packet k{}; k.magic = MAGIC; k.version = PROTO; k.sender = 0xABCD; k.mission = 4;
    for (int i = 0; i < 3; ++i) { k.seq = i + 1; k.s = MakeSummary(r, u8"Друг <Фенсер>"); sendto(o, (char *)&k, sizeof k, 0, (sockaddr *)&to, sizeof to); Sleep(30); }
    Sleep(100);
    EnterCriticalSection(&g_cs); NetReceive(); LeaveCriticalSection(&g_cs);

    // remote Ranger over the fake EOS connection: our packet + a game packet in the same receive call
    g_useEos = true; g_eosSock.ApiVersion = 1; strcpy(g_eosSock.SocketName, EOS_SOCKET);
    oEosRecv = FakeRecv; oEosSend = FakeSend; pEosAccept = FakeAccept;
    EOS_PUID ranger = (EOS_PUID)0x5000, noPlugin = (EOS_PUID)0x6000, local = (EOS_PUID)0x7000;
    // the game sends to both players -> plugin learns them
    EOS_SocketId gameSock{ 1, "GAME" }; EOS_SendOpts gs{}; gs.ApiVersion = 3; gs.LocalUserId = local; gs.SocketId = &gameSock;
    gs.RemoteUserId = ranger; hEosSend((EOS_HP2P)0x1, &gs); gs.RemoteUserId = noPlugin; hEosSend((EOS_HP2P)0x1, &gs);
    PStats rg; rg.cls = "AssultSoldier"; rg.dmg = 31000; rg.kills = 40; rg.killsBy[u8"Берсеркер"] = 40; rg.dmgBy[u8"Берсеркер"] = 31000; rg.dist = 2100;
    Packet e{}; e.magic = MAGIC; e.version = PROTO; e.sender = 0xBEEF; e.mission = 9; e.seq = 1; e.s = MakeSummary(rg, u8"Рейнджер Вася");
    q_in.push_back({ ranger, "EDFStats", std::vector<uint8_t>((uint8_t *)&e, (uint8_t *)&e + sizeof e) });
    q_in.push_back({ ranger, "GAME", { 1, 2, 3 } });
    EOS_RecvOpts ro{ 2, local, 0x1000, nullptr };
    EOS_PUID outPeer; EOS_SocketId outSock{ 1, "" }; uint8_t outCh; static uint8_t buf[0x1000]; uint32_t outBytes = 0;
    int32_t res = hEosRecv((EOS_HP2P)0x1, &ro, &outPeer, &outSock, &outCh, buf, &outBytes);
    printf("game got: result=%d socket=%s bytes=%u (expect 0 GAME 3)\n", res, outSock.SocketName, outBytes);
    res = hEosRecv((EOS_HP2P)0x1, &ro, &outPeer, &outSock, &outCh, buf, &outBytes);
    printf("then: result=%d (expect 13), accepted=%d (expect 2), sent=%d (expect 4: 2 by the game + 2 by EDFStats) on socket %s channel %u\n",
           res, accepted, sent, lastSentSock.c_str(), lastSentCh);

    EnterCriticalSection(&g_cs); WriteResults("test"); LeaveCriticalSection(&g_cs);
    printf("remotes=%zu (expect 2)\n", g_remotes.size());
    // mission matching scenarios
    { Remote &rm = g_remotes[0xABCD]; Summary old = rm.byMission[4]; old.dmg = 99999; rm.byMission[3] = old;
      uint64_t now = GetTickCount64(); rm.span[3] = { g_missionStartMs + 1000, now - 2000 };
      auto v = RemoteSummaries(g_missionStartMs, now); double got = -1; for (auto &s : v) if (!strcmp(s.cls, "Fencer")) got = s.dmg;
      printf("scenario2 picked dmg=%.0f (expect 99999)\n", got);
      auto w = RemoteSummaries(now - 500, now); got = -1; for (auto &s : w) if (!strcmp(s.cls, "Fencer")) got = s.dmg;
      printf("scenario3 picked dmg=%.0f (expect 25000)\n", got); }
    { std::vector<Row> rows; }
    return 0;
}
