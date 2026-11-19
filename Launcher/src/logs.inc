// ---- logs: collect them into one zip, send it to the other players on the LAN, receive theirs -------------------
// (included from launcher.cpp after the state / config section)
//  * "Collect logs": every mod's log and ini, ModLoader.log, the launcher's settings, the latest EDFStats report and the
//    last missions' reports + info.txt -> <game>\EDF6Launcher_logs\EDF6-logs_<player>_<time>.zip, the folder is opened.
//  * "Send logs": the same zip goes over TCP to every address in Peers (LogPeers in EDF6Launcher.ini, else the Peers line
//    of EDFStats.ini or SquadSync.ini); this PC's own addresses are skipped.
//  * While the launcher is open it listens on LogPort (27788) and saves what arrives to EDF6Launcher_logs\received.
//    Only a zip of at most 100 MB is accepted; nothing received is ever opened or run.
// Wire format (one file per connection): "EDF6LOGS", u32 version 1, u32 name length, name (UTF-8 file name),
// u64 size, data; the receiver answers "OK!!" after the file is on disk.
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")

static int g_logPort = 27788;
static bool g_receiveLogs = true;
static std::string g_logPeersSetting;                    // LogPeers= from EDF6Launcher.ini (empty = from the mods' ini files)
static std::string g_listenState;                         // shown in the settings panel
static std::atomic<int> g_receivedCount{ 0 };             // files received since the launcher started
static std::string g_lastReceived;                        // guarded by g_mx

static std::string LogsDir() { return g_gameDir + "\\EDF6Launcher_logs"; }
static std::string ReceivedDir() { return LogsDir() + "\\received"; }

// ---- zip writer (stored, no compression: logs are small and every unzip tool reads it) ---------------------------
struct ZipEntry { std::string name, data; };
static uint32_t Crc32(const std::string &d) {
    static uint32_t tab[256]; static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; tab[i] = c; } init = true; }
    uint32_t c = 0xFFFFFFFFu;
    for (unsigned char ch : d) c = tab[(c ^ ch) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
static void Put16(std::string &o, uint16_t v) { o += (char)(v & 0xFF); o += (char)(v >> 8); }
static void Put32(std::string &o, uint32_t v) { Put16(o, (uint16_t)(v & 0xFFFF)); Put16(o, (uint16_t)(v >> 16)); }
static std::string MakeZip(const std::vector<ZipEntry> &files) {
    SYSTEMTIME st; GetLocalTime(&st);
    uint16_t dosTime = (uint16_t)(st.wHour << 11 | st.wMinute << 5 | st.wSecond / 2);
    uint16_t dosDate = (uint16_t)((st.wYear - 1980) << 9 | st.wMonth << 5 | st.wDay);
    std::string out, cd;
    for (auto &f : files) {
        uint32_t crc = Crc32(f.data), size = (uint32_t)f.data.size(), off = (uint32_t)out.size();
        Put32(out, 0x04034b50); Put16(out, 20); Put16(out, 0x0800 /* UTF-8 names */); Put16(out, 0 /* stored */);
        Put16(out, dosTime); Put16(out, dosDate); Put32(out, crc); Put32(out, size); Put32(out, size);
        Put16(out, (uint16_t)f.name.size()); Put16(out, 0); out += f.name; out += f.data;
        Put32(cd, 0x02014b50); Put16(cd, 20); Put16(cd, 20); Put16(cd, 0x0800); Put16(cd, 0); Put16(cd, dosTime); Put16(cd, dosDate);
        Put32(cd, crc); Put32(cd, size); Put32(cd, size); Put16(cd, (uint16_t)f.name.size()); Put16(cd, 0); Put16(cd, 0);
        Put16(cd, 0); Put16(cd, 0); Put32(cd, 0); Put32(cd, off); cd += f.name;
    }
    uint32_t cdOff = (uint32_t)out.size();
    out += cd;
    Put32(out, 0x06054b50); Put16(out, 0); Put16(out, 0); Put16(out, (uint16_t)files.size()); Put16(out, (uint16_t)files.size());
    Put32(out, (uint32_t)cd.size()); Put32(out, cdOff); Put16(out, 0);
    return out;
}

// ---- what goes into the archive --------------------------------------------------------------------------------
static std::string IniValue(const std::string &file, const char *section, const char *key) {
    wchar_t b[1024]; GetPrivateProfileStringW(W(section).c_str(), W(key).c_str(), L"", b, 1024, W(file).c_str()); return U(b);
}
static std::string PlayerName() {       // EDFStats.ini Name=, else the Steam name EDFStats logged, else "player"
    std::string n = IniValue(g_plugins + "\\EDFStats.ini", "EDFStats", "Name");
    if (n.empty()) {
        std::string log; const std::string tag = "player name from Steam: ";
        if (ReadFile(g_plugins + "\\EDFStats.log", log)) {
            size_t p = log.rfind(tag);
            if (p != std::string::npos) { size_t e = log.find_first_of("\r\n", p); n = log.substr(p + tag.size(), e == std::string::npos ? std::string::npos : e - p - tag.size()); }
        }
    }
    return n.empty() ? "player" : n;
}
static std::string SafeFileName(std::string s) {
    for (char &c : s) if (strchr("\\/:*?\"<>|", c) || (unsigned char)c < 0x20) c = '_';
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    return s.empty() ? "file" : s;
}
static std::vector<std::pair<std::string, FILETIME>> ListFiles(const std::string &dir, const char *pattern) {
    std::vector<std::pair<std::string, FILETIME>> v; WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(W(dir + "\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) v.push_back({ U(fd.cFileName), fd.ftLastWriteTime }); } while (FindNextFileW(h, &fd));
    FindClose(h); return v;
}
static std::vector<ZipEntry> CollectLogFiles() {
    std::vector<ZipEntry> z; std::string info;
    auto add = [&](const std::string &path, const std::string &inZip) {
        std::string d;
        if (ReadFile(path, d) && d.size() <= 20u * 1024 * 1024) { z.push_back({ inZip, d }); info += "  " + inZip + " (" + std::to_string(d.size()) + " bytes)\r\n"; }
    };
    for (auto &f : ListFiles(g_plugins, "*.log")) add(g_plugins + "\\" + f.first, "logs/" + f.first);
    add(g_gameDir + "\\ModLoader.log", "logs/ModLoader.log");
    for (auto &f : ListFiles(g_plugins, "*.ini")) add(g_plugins + "\\" + f.first, "settings/" + f.first);
    add(g_lockPath, "settings/edf6mods.lock.json");
    add(g_gameDir + "\\ModLoader.ini", "settings/ModLoader.ini");
    add(g_cfgPath, "settings/EDF6Launcher.ini");
    add(g_plugins + "\\EDFStats\\latest.html", "reports/latest.html");
    auto missions = ListFiles(g_plugins + "\\EDFStats\\missions", "*.*");      // the newest 3 missions (html + json)
    std::sort(missions.begin(), missions.end(), [](auto &a, auto &b) { return CompareFileTime(&a.second, &b.second) > 0; });
    for (size_t i = 0; i < missions.size() && i < 6; ++i) add(g_plugins + "\\EDFStats\\missions\\" + missions[i].first, "reports/missions/" + missions[i].first);
    SYSTEMTIME st; GetLocalTime(&st); char when[64];
    snprintf(when, sizeof when, "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string head = "EDF6 Launcher " + std::string(LAUNCHER_VERSION) + " logs\r\nplayer: " + PlayerName() + "\r\ncollected: " + when +
                       "\r\ngame folder: " + g_gameDir + "\r\ngame running: " + (GameRunning() ? "yes" : "no") + "\r\nmods:\r\n";
    { std::lock_guard<std::mutex> l(g_mx);
      if (g_mods.empty()) ScanInstalled();                    // mod list not loaded yet: at least what is in Mods\Plugins
      for (auto &m : g_mods) if (m.present || m.inManifest)
          head += "  " + m.id + " installed=" + (m.installed.empty() ? "-" : m.installed) + " latest=" + (m.version.empty() ? "-" : m.version) +
                  (m.present ? (m.enabled ? " on" : " off") : " not installed") + "\r\n"; }
    z.insert(z.begin(), { "info.txt", head + "files:\r\n" + info });
    return z;
}
// builds the archive; returns its path ("" on failure)
static std::string BuildLogZip(std::string &zipData) {
    std::vector<ZipEntry> files = CollectLogFiles();
    zipData = MakeZip(files);
    SYSTEMTIME st; GetLocalTime(&st); char stamp[64];
    snprintf(stamp, sizeof stamp, "%04d-%02d-%02d_%02d-%02d-%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    CreateDirectoryW(W(LogsDir()).c_str(), nullptr);
    std::string path = LogsDir() + "\\" + SafeFileName("EDF6-logs_" + PlayerName() + "_" + stamp + ".zip");
    return WriteFileAtomic(path, zipData) ? path : "";
}
static void ShowInExplorer(const std::string &file) {
    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + W(file) + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

// ---- network ---------------------------------------------------------------------------------------------------
static bool WsaUp() { static bool ok = [] { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }(); return ok; }
static bool SendAll(SOCKET s, const char *p, size_t n) {
    while (n) { int k = send(s, p, (int)std::min<size_t>(n, 1 << 20), 0); if (k <= 0) return false; p += k; n -= k; }
    return true;
}
static bool RecvAll(SOCKET s, char *p, size_t n) {
    while (n) { int k = recv(s, p, (int)std::min<size_t>(n, 1 << 20), 0); if (k <= 0) return false; p += k; n -= k; }
    return true;
}
static std::vector<std::string> SplitPeers(const std::string &line) {
    std::vector<std::string> v; std::string cur;
    for (char c : line + ",") {
        if (c == ',' || c == ';' || c == ' ' || c == '\t') { if (!cur.empty()) { size_t k = cur.find(':'); v.push_back(k == std::string::npos ? cur : cur.substr(0, k)); cur.clear(); } }
        else cur += c;
    }
    return v;
}
static std::vector<std::string> LogPeers() {
    std::string line = g_logPeersSetting;
    if (line.empty()) line = IniValue(g_plugins + "\\EDFStats.ini", "EDFStats", "Peers");
    if (line.empty()) line = IniValue(g_plugins + "\\SquadSync.ini", "SquadSync", "Peers");
    return SplitPeers(line);
}
static std::vector<std::string> OwnAddresses() {          // this PC's IPv4 addresses (LAN, VPN adapters ...)
    std::vector<std::string> v; char host[256] = {};
    if (!WsaUp() || gethostname(host, sizeof host) != 0) return v;
    addrinfo hints{}, *res = nullptr; hints.ai_family = AF_INET;
    if (getaddrinfo(host, nullptr, &hints, &res) == 0) {
        for (addrinfo *a = res; a; a = a->ai_next) { char b[64]; inet_ntop(AF_INET, &((sockaddr_in *)a->ai_addr)->sin_addr, b, sizeof b); v.push_back(b); }
        freeaddrinfo(res);
    }
    v.push_back("127.0.0.1");
    return v;
}
// one file to one address; "" = delivered, otherwise the reason
static std::string SendLogFile(const std::string &ip, int port, const std::string &name, const std::string &data) {
    if (!WsaUp()) return "WSAStartup";
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) return L_(u8"неверный адрес", u8"bad address");
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP); if (s == INVALID_SOCKET) return "socket";
    u_long nb = 1; ioctlsocket(s, FIONBIO, &nb);
    connect(s, (sockaddr *)&a, sizeof a);
    fd_set w; FD_ZERO(&w); FD_SET(s, &w); fd_set e = w; timeval tv{ 3, 0 };
    int r = select(0, nullptr, &w, &e, &tv);
    if (r <= 0 || FD_ISSET(s, &e)) { closesocket(s); return L_(u8"нет ответа (лаунчер закрыт или брандмауэр)", u8"no answer (launcher closed or firewall)"); }
    nb = 0; ioctlsocket(s, FIONBIO, &nb);
    DWORD to = 20000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to); setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
    std::string head = "EDF6LOGS"; uint32_t ver = 1, nl = (uint32_t)name.size(); uint64_t sz = data.size();
    head.append((const char *)&ver, 4); head.append((const char *)&nl, 4); head += name; head.append((const char *)&sz, 8);
    char ok[4] = {};
    bool good = SendAll(s, head.data(), head.size()) && SendAll(s, data.data(), data.size()) && RecvAll(s, ok, 4) && !memcmp(ok, "OK!!", 4);
    closesocket(s);
    return good ? "" : L_(u8"передача прервалась", u8"transfer failed");
}
// collects, saves and sends to every peer; returns the status line
static std::string SendLogs() {
    std::string data, path = BuildLogZip(data);
    if (path.empty()) return L_(u8"Не удалось сохранить архив логов", u8"Cannot save the log archive");
    std::string name = path.substr(path.find_last_of('\\') + 1);
    std::vector<std::string> peers = LogPeers(), own = OwnAddresses();
    std::string out; int sent = 0, tried = 0;
    for (auto &ip : peers) {
        if (std::find(own.begin(), own.end(), ip) != own.end()) continue;    // ourselves
        ++tried;
        std::string err = SendLogFile(ip, g_logPort, name, data);
        out += "  " + ip + (err.empty() ? L_(u8" — доставлено", u8" - delivered") : " - " + err);
        if (err.empty()) ++sent;
    }
    if (!tried) return L_(u8"Архив сохранён, но адресов нет: заполните Peers в EDFStats.ini или LogPeers в EDF6Launcher.ini", u8"Archive saved, but there are no addresses: fill in Peers in EDFStats.ini or LogPeers in EDF6Launcher.ini");
    return std::string(L_(u8"Логи отправлены: ", u8"Logs sent: ")) + std::to_string(sent) + "/" + std::to_string(tried) + "." + out;
}

// ---- receiving ---------------------------------------------------------------------------------------------------
static void HandleLogConnection(SOCKET c, const std::string &from) {
    DWORD to = 20000; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to); setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof to);
    char magic[8]; uint32_t ver = 0, nl = 0; uint64_t sz = 0;
    if (!RecvAll(c, magic, 8) || memcmp(magic, "EDF6LOGS", 8) || !RecvAll(c, (char *)&ver, 4) || ver != 1 || !RecvAll(c, (char *)&nl, 4) || nl == 0 || nl > 200) return;
    std::string name(nl, 0);
    if (!RecvAll(c, &name[0], nl) || !RecvAll(c, (char *)&sz, 8) || sz == 0 || sz > 100ull * 1024 * 1024) return;
    std::string data((size_t)sz, 0);
    if (!RecvAll(c, &data[0], (size_t)sz) || data.compare(0, 4, "PK\x03\x04", 4) != 0) return;     // zip archives only
    name = SafeFileName(name);
    if (name.size() < 4 || _stricmp(name.c_str() + name.size() - 4, ".zip") != 0) name += ".zip";
    CreateDirectoryW(W(LogsDir()).c_str(), nullptr); CreateDirectoryW(W(ReceivedDir()).c_str(), nullptr);
    std::string path = ReceivedDir() + "\\" + name;
    for (int k = 2; Exists(path) && k < 100; ++k) path = ReceivedDir() + "\\" + name.substr(0, name.size() - 4) + " (" + std::to_string(k) + ").zip";
    if (!WriteFileAtomic(path, data)) return;
    SendAll(c, "OK!!", 4);
    ++g_receivedCount;
    std::lock_guard<std::mutex> l(g_mx);
    g_lastReceived = name;
    g_status = L_(u8"Получены логи: ", u8"Logs received: ") + name + " (" + from + ")";
}
static void StartLogListener() {
    if (!g_receiveLogs) { g_listenState = L_(u8"приём логов выключен (ReceiveLogs=0)", u8"receiving logs is off (ReceiveLogs=0)"); return; }
    if (!WsaUp()) { g_listenState = "WSAStartup"; return; }
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons((u_short)g_logPort); a.sin_addr.s_addr = INADDR_ANY;
    BOOL excl = TRUE; setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof excl);
    if (s == INVALID_SOCKET || bind(s, (sockaddr *)&a, sizeof a) != 0 || listen(s, 4) != 0) {
        if (s != INVALID_SOCKET) closesocket(s);
        g_listenState = L_(u8"порт ", u8"port ") + std::to_string(g_logPort) + L_(u8" занят — логи сейчас не принимаются", u8" is busy - logs are not received now");
        return;
    }
    g_listenState = L_(u8"принимаю логи на порту ", u8"receiving logs on port ") + std::to_string(g_logPort);
    std::thread([s] {
        for (;;) {
            sockaddr_in from{}; int fl = sizeof from;
            SOCKET c = accept(s, (sockaddr *)&from, &fl);
            if (c == INVALID_SOCKET) { Sleep(200); continue; }
            char ip[64] = "?"; inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            HandleLogConnection(c, ip);
            closesocket(c);
        }
    }).detach();
}
static void LoadLogConfig() {
    auto get = [&](const wchar_t *k, const char *def) {
        wchar_t b[1024]; GetPrivateProfileStringW(L"Launcher", k, W(def).c_str(), b, 1024, W(g_cfgPath).c_str()); return U(b);
    };
    g_logPort = atoi(get(L"LogPort", "27788").c_str()); if (g_logPort <= 0 || g_logPort > 65535) g_logPort = 27788;
    g_receiveLogs = get(L"ReceiveLogs", "1") != "0";
    g_logPeersSetting = get(L"LogPeers", "");
}

// ---- UI pieces (called with g_mx held) -------------------------------------------------------------------------
static void LogButtons(bool busy) {
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(L_(u8"Собрать логи", u8"Collect logs")))
        RunWorker([] { Status(L_(u8"Собираю логи...", u8"Collecting logs...")); std::string d, p = BuildLogZip(d);
                       if (p.empty()) Status(L_(u8"Не удалось сохранить архив логов", u8"Cannot save the log archive"));
                       else { ShowInExplorer(p); Status(L_(u8"Логи собраны: ", u8"Logs collected: ") + p); } });
    ImGui::SameLine();
    if (ImGui::Button(L_(u8"Отправить логи друзьям", u8"Send logs to friends")))
        RunWorker([] { Status(L_(u8"Собираю и отправляю логи...", u8"Collecting and sending logs...")); Status(SendLogs()); });
    ImGui::EndDisabled();
    int n = g_receivedCount;
    if (n > 0 || Exists(ReceivedDir())) {
        ImGui::SameLine();
        std::string label = std::string(L_(u8"Полученные логи", u8"Received logs")) + (n ? " (+" + std::to_string(n) + ")" : "");
        if (ImGui::Button(label.c_str())) { CreateDirectoryW(W(LogsDir()).c_str(), nullptr); CreateDirectoryW(W(ReceivedDir()).c_str(), nullptr); OpenPath(ReceivedDir()); }
    }
}
