// EDF6 Launcher 0.1 (draft) - small launcher for EDF6 mods.
//   * mod list from mods.json in the GitHub repository (version, download links, SHA-256)
//   * install / update / remove, on/off (off = DLL moved to Mods\Plugins\disabled, EDFModLoader does not load subfolders)
//   * ini editor for every mod (comments become hints, 0/1 become checkboxes, keys are captured by pressing them)
//   * checks EDFModLoader, opens logs / reports / folders, starts the game
// Updates never overwrite the player's ini: only keys that are missing are added.
// Command line (for testing): --game-dir <dir>  --manifest <file or url>  --screenshot <file.bmp>  --selftest-install
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static const char *LAUNCHER_VERSION = "0.2";
static const char *DEFAULT_MANIFEST = "https://raw.githubusercontent.com/don-pardon42/edf6-mods/main/mods.json";

// ---- small helpers ------------------------------------------------------------------------------
static std::wstring W(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n); return w;
}
static std::string U(const std::wstring &w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr); return s;
}
static bool Exists(const std::string &p) { return GetFileAttributesW(W(p).c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool ReadFile(const std::string &p, std::string &out) {
    FILE *f = _wfopen(W(p).c_str(), L"rb"); if (!f) return false;
    char buf[65536]; size_t n; out.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f); return true;
}
static bool WriteFileAtomic(const std::string &p, const std::string &data) {
    std::string tmp = p + ".new";
    FILE *f = _wfopen(W(tmp).c_str(), L"wb"); if (!f) return false;
    bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (fclose(f) == 0) && ok;
    if (!ok) { DeleteFileW(W(tmp).c_str()); return false; }
    return MoveFileExW(W(tmp).c_str(), W(p).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
static std::string ExeDir() {
    wchar_t b[MAX_PATH]; GetModuleFileNameW(nullptr, b, MAX_PATH);
    std::wstring s(b); size_t k = s.find_last_of(L"\\/"); return U(s.substr(0, k));
}
static void OpenPath(const std::string &p) { ShellExecuteW(nullptr, L"open", W(p).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
static bool ProcessRunning(const wchar_t *exe) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); if (s == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{ sizeof e }; bool found = false;
    for (BOOL ok = Process32FirstW(s, &e); ok; ok = Process32NextW(s, &e)) if (!_wcsicmp(e.szExeFile, exe)) { found = true; break; }
    CloseHandle(s); return found;
}

// ---- minimal JSON (enough for mods.json and the lock file) ----------------------------------------------
struct Json {
    enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t = NUL;
    bool b = false; double n = 0; std::string s; std::vector<Json> a; std::vector<std::pair<std::string, Json>> o;
    const Json &operator[](const char *k) const { static Json none; for (auto &kv : o) if (kv.first == k) return kv.second; return none; }
    std::string str(const char *def = "") const { return t == STR ? s : def; }
};
struct JsonParser {
    const char *p, *e; bool ok = true;
    void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p; }
    static void put(std::string &o, unsigned c) {
        if (c < 0x80) o += (char)c;
        else if (c < 0x800) { o += (char)(0xC0 | c >> 6); o += (char)(0x80 | (c & 63)); }
        else if (c < 0x10000) { o += (char)(0xE0 | c >> 12); o += (char)(0x80 | (c >> 6 & 63)); o += (char)(0x80 | (c & 63)); }
        else { o += (char)(0xF0 | c >> 18); o += (char)(0x80 | (c >> 12 & 63)); o += (char)(0x80 | (c >> 6 & 63)); o += (char)(0x80 | (c & 63)); }
    }
    std::string str() {
        std::string o; ++p;
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                char c = p[1]; p += 2;
                if (c == 'n') o += '\n'; else if (c == 't') o += '\t'; else if (c == 'r') o += '\r'; else if (c == 'b') o += '\b'; else if (c == 'f') o += '\f';
                else if (c == 'u' && p + 4 <= e) {
                    unsigned u = strtoul(std::string(p, 4).c_str(), nullptr, 16); p += 4;
                    if (u >= 0xD800 && u < 0xDC00 && p + 6 <= e && p[0] == '\\' && p[1] == 'u') {
                        unsigned lo = strtoul(std::string(p + 2, 4).c_str(), nullptr, 16); p += 6; u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put(o, u);
                } else o += c;
            } else o += *p++;
        }
        if (p < e) ++p; else ok = false;
        return o;
    }
    Json val() {
        Json j; ws(); if (p >= e) { ok = false; return j; }
        if (*p == '{') {
            j.t = Json::OBJ; ++p; ws();
            if (p < e && *p == '}') { ++p; return j; }
            while (ok && p < e) {
                ws(); if (*p != '"') { ok = false; break; }
                std::string k = str(); ws(); if (p >= e || *p != ':') { ok = false; break; } ++p;
                j.o.push_back({ k, val() }); ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; break; }
                ok = false;
            }
        } else if (*p == '[') {
            j.t = Json::ARR; ++p; ws();
            if (p < e && *p == ']') { ++p; return j; }
            while (ok && p < e) {
                j.a.push_back(val()); ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; break; }
                ok = false;
            }
        } else if (*p == '"') { j.t = Json::STR; j.s = str(); }
        else if (!strncmp(p, "true", 4)) { j.t = Json::BOOL; j.b = true; p += 4; }
        else if (!strncmp(p, "false", 5)) { j.t = Json::BOOL; p += 5; }
        else if (!strncmp(p, "null", 4)) { p += 4; }
        else { char *end; j.t = Json::NUM; j.n = strtod(p, &end); if (end == p) ok = false; p = end; }
        return j;
    }
};
static bool ParseJson(const std::string &text, Json &out) {
    JsonParser jp{ text.data(), text.data() + text.size() };
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF) jp.p += 3;       // UTF-8 BOM
    out = jp.val(); return jp.ok;
}
static std::string JsonStr(const std::string &s) {
    std::string o = "\"";
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if ((unsigned char)c >= 0x20) o += c; }
    return o + "\"";
}

// ---- HTTPS download (WinHTTP, follows GitHub's redirects) and SHA-256 (CNG) ------------------------------
static bool HttpGet(const std::string &url, std::string &out, std::string &err) {
    std::wstring wu = W(url);
    URL_COMPONENTS uc{ sizeof uc }; wchar_t host[256], path[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256; uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wu.c_str(), 0, 0, &uc)) { err = "bad url"; return false; }
    HINTERNET s = WinHttpOpen(L"EDF6Launcher/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) s = WinHttpOpen(L"EDF6Launcher/0.1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { err = "WinHttpOpen failed"; return false; }
    WinHttpSetTimeouts(s, 5000, 5000, 10000, 20000);
    HINTERNET c = WinHttpConnect(s, host, uc.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr);
    DWORD status = 0, sz = sizeof status;
    if (ok) WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
    if (ok && status == 200) {
        out.clear();
        for (;;) {
            DWORD avail = 0; if (!WinHttpQueryDataAvailable(r, &avail) || !avail) break;
            size_t at = out.size(); out.resize(at + avail); DWORD got = 0;
            if (!WinHttpReadData(r, &out[at], avail, &got)) { ok = false; break; }
            out.resize(at + got);
        }
    } else { if (ok) err = "HTTP " + std::to_string(status); else err = "no connection (" + std::to_string(GetLastError()) + ")"; ok = false; }
    if (r) WinHttpCloseHandle(r); if (c) WinHttpCloseHandle(c); WinHttpCloseHandle(s);
    return ok;
}
static std::string Sha256Hex(const std::string &data) {
    unsigned char h[32]; char hex[65];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), h, 32) != 0) return "";
    for (int i = 0; i < 32; ++i) sprintf(hex + i * 2, "%02x", h[i]);
    return hex;
}

// ---- ini files: kept line by line so comments and order survive editing --------------------------------
struct IniLine { std::string raw, key, value, hint; bool isKey = false; };
struct IniFile { std::string path; std::vector<IniLine> lines; bool dirty = false; };
static bool LoadIni(const std::string &path, IniFile &ini) {
    std::string text; if (!ReadFile(path, text)) return false;
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF) text = text.substr(3);
    ini.path = path; ini.lines.clear(); ini.dirty = false;
    std::string hint; size_t i = 0;
    while (i <= text.size()) {
        size_t j = text.find('\n', i); if (j == std::string::npos) j = text.size();
        std::string ln = text.substr(i, j - i); if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        IniLine L; L.raw = ln;
        size_t a = ln.find_first_not_of(" \t");
        if (a != std::string::npos && (ln[a] == ';' || ln[a] == '#')) { hint += (hint.empty() ? "" : "\n") + ln.substr(a + 1); }
        else if (a != std::string::npos && ln[a] != '[' && ln.find('=') != std::string::npos) {
            size_t eq = ln.find('=');
            L.isKey = true; L.key = ln.substr(a, eq - a); L.value = ln.substr(eq + 1);
            while (!L.key.empty() && (L.key.back() == ' ' || L.key.back() == '\t')) L.key.pop_back();
            L.hint = hint; hint.clear();
        } else hint.clear();
        ini.lines.push_back(L);
        if (j >= text.size()) break;
        i = j + 1;
    }
    return true;
}
static bool SaveIni(IniFile &ini) {
    std::string out;
    for (size_t i = 0; i < ini.lines.size(); ++i) {
        auto &L = ini.lines[i];
        out += L.isKey ? L.key + "=" + L.value : L.raw;
        if (i + 1 < ini.lines.size()) out += "\r\n";
    }
    if (!WriteFileAtomic(ini.path, out)) return false;
    ini.dirty = false; return true;
}
// add the keys of a newer default ini that the player's ini does not have yet (with their comments)
static int MergeIni(const std::string &userPath, const std::string &defaults) {
    std::string user; if (!ReadFile(userPath, user)) return -1;
    std::map<std::string, bool> have;
    IniFile u; LoadIni(userPath, u); for (auto &L : u.lines) if (L.isKey) have[L.key] = true;
    std::string add, pending; size_t i = 0; int n = 0;
    while (i <= defaults.size()) {
        size_t j = defaults.find('\n', i); if (j == std::string::npos) j = defaults.size();
        std::string ln = defaults.substr(i, j - i); if (!ln.empty() && ln.back() == '\r') ln.pop_back();
        size_t a = ln.find_first_not_of(" \t");
        if (a != std::string::npos && (ln[a] == ';' || ln[a] == '#')) pending += ln + "\r\n";
        else if (a != std::string::npos && ln[a] != '[' && ln.find('=') != std::string::npos) {
            std::string k = ln.substr(a, ln.find('=') - a); while (!k.empty() && k.back() == ' ') k.pop_back();
            if (!have.count(k)) { add += pending + ln + "\r\n"; ++n; }
            pending.clear();
        } else pending.clear();
        if (j >= defaults.size()) break;
        i = j + 1;
    }
    if (!n) return 0;
    if (!user.empty() && user.back() != '\n') user += "\r\n";
    user += "; ---- added by EDF6 Launcher (new in this version) ----\r\n" + add;
    return WriteFileAtomic(userPath, user) ? n : -1;
}

// ---- state ---------------------------------------------------------------------------------------------
struct ModFile { std::string name, kind, url, sha; };
struct Mod {
    std::string id, version, description, log, report; std::vector<ModFile> files;   // from mods.json
    bool inManifest = false;
    std::string installed;        // version from the lock file, "?" = DLL present but not installed by the launcher
    bool present = false, enabled = false;
};
static std::string g_gameDir, g_plugins, g_disabled, g_lockPath, g_cfgPath, g_manifestSrc = DEFAULT_MANIFEST;
static std::string g_gameExe = "EDF6.exe", g_gameArgs;
static bool g_closeOnPlay = false;
static int g_theme = 0;                 // index into g_themes (UI section)
static std::vector<Mod> g_mods;
static std::string g_modloaderPage = "https://github.com/BlueAmulet/EDFModLoader/releases";
static std::mutex g_mx;                                  // guards g_mods / g_status while a worker runs
static std::string g_status = u8"Готово";
static std::atomic<bool> g_busy{ false };
static bool g_manifestOk = false; static std::string g_manifestErr;
static std::map<std::string, std::string> g_lock;       // id -> version installed by the launcher

static void Status(const std::string &s) { std::lock_guard<std::mutex> l(g_mx); g_status = s; }
static void LoadLock() {
    g_lock.clear(); std::string t; Json j;
    if (ReadFile(g_lockPath, t) && ParseJson(t, j)) for (auto &kv : j["installed"].o) g_lock[kv.first] = kv.second["version"].str();
}
static void SaveLock() {
    std::string o = "{\n  \"installed\": {";
    bool first = true;
    for (auto &kv : g_lock) { o += std::string(first ? "\n" : ",\n") + "    " + JsonStr(kv.first) + ": {\"version\": " + JsonStr(kv.second) + "}"; first = false; }
    o += "\n  }\n}\n";
    WriteFileAtomic(g_lockPath, o);
}
static std::string DllOf(const Mod &m) { for (auto &f : m.files) if (f.kind == "dll") return f.name; return m.id + ".dll"; }
static std::string IniOf(const Mod &m) { for (auto &f : m.files) if (f.kind == "ini") return f.name; return m.id + ".ini"; }
static void ScanInstalled() {                           // caller holds g_mx
    LoadLock();
    for (auto &m : g_mods) {
        std::string dll = DllOf(m);
        bool on = Exists(g_plugins + "\\" + dll), off = Exists(g_disabled + "\\" + dll);
        m.present = on || off; m.enabled = on;
        auto it = g_lock.find(m.id);
        m.installed = m.present ? (it != g_lock.end() ? it->second : "?") : "";
    }
    // DLLs in Mods\Plugins that are not in the list (other people's mods) are shown too
    WIN32_FIND_DATAW fd;
    for (int pass = 0; pass < 2; ++pass) {
        std::string dir = pass ? g_disabled : g_plugins;
        HANDLE h = FindFirstFileW(W(dir + "\\*.dll").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::string n = U(fd.cFileName), id = n.substr(0, n.size() - 4);
            bool known = false; for (auto &m : g_mods) if (!_stricmp(DllOf(m).c_str(), n.c_str())) known = true;
            if (known) continue;
            Mod m; m.id = id; m.present = true; m.enabled = pass == 0; m.installed = "?";
            m.files.push_back({ n, "dll", "", "" }); m.files.push_back({ id + ".ini", "ini", "", "" }); m.log = id + ".log";
            g_mods.push_back(m);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
}
static void LoadManifest() {
    std::string text, err; Json j;
    bool ok = g_manifestSrc.rfind("http", 0) == 0 ? HttpGet(g_manifestSrc, text, err) : (ReadFile(g_manifestSrc, text) || ((err = "file not found"), false));
    std::vector<Mod> list;
    if (ok && ParseJson(text, j)) {
        if (!j["modloader"]["page"].str().empty()) g_modloaderPage = j["modloader"]["page"].str();
        for (auto &e : j["mods"].a) {
            Mod m; m.id = e["id"].str(); m.version = e["version"].str(); m.description = e["description"].str();
            m.log = e["log"].str(); m.report = e["report"].str(); m.inManifest = true;
            for (auto &f : e["files"].a) m.files.push_back({ f["name"].str(), f["kind"].str(), f["url"].str(), f["sha256"].str() });
            if (!m.id.empty()) list.push_back(m);
        }
    } else if (ok) { ok = false; err = "mods.json is damaged"; }
    std::lock_guard<std::mutex> l(g_mx);
    g_manifestOk = ok; g_manifestErr = err;
    if (ok) g_mods = list;
    else {   // offline: keep what we know from the lock file / Plugins folder
        std::vector<Mod> keep; for (auto &m : g_mods) if (m.inManifest) keep.push_back(m);
        g_mods = keep;
    }
    ScanInstalled();
    g_status = ok ? u8"Список модов обновлён" : u8"Нет связи с GitHub: " + err + u8" — показаны установленные моды";
}

// ---- operations (run on a worker thread) -----------------------------------------------------------------
static bool GameRunning() { return ProcessRunning(W(g_gameExe.substr(g_gameExe.find_last_of("\\/") + 1)).c_str()) || ProcessRunning(L"EDF6.exe"); }
static bool InstallMod(Mod m, std::string &msg) {
    if (GameRunning()) { msg = u8"Закройте игру перед установкой"; return false; }
    CreateDirectoryW(W(g_gameDir + "\\Mods").c_str(), nullptr); CreateDirectoryW(W(g_plugins).c_str(), nullptr);
    std::map<std::string, std::string> data;
    for (auto &f : m.files) {
        if (f.url.empty()) continue;
        Status(u8"Скачиваю " + f.name + "...");
        std::string body, err;
        if (!HttpGet(f.url, body, err)) { msg = f.name + ": " + err; return false; }
        if (!f.sha.empty() && Sha256Hex(body) != f.sha) { msg = f.name + u8": контрольная сумма не совпала, файл не установлен"; return false; }
        data[f.name] = body;
    }
    bool disabled = m.present && !m.enabled;
    int added = 0;
    for (auto &f : m.files) {
        if (!data.count(f.name)) continue;
        if (f.kind == "dll") {
            std::string dir = disabled ? g_disabled : g_plugins, path = dir + "\\" + f.name;
            if (disabled) CreateDirectoryW(W(g_disabled).c_str(), nullptr);
            if (Exists(path)) CopyFileW(W(path).c_str(), W(path + ".bak").c_str(), FALSE);
            if (!WriteFileAtomic(path, data[f.name])) { msg = f.name + u8": не удалось записать (файл занят?)"; return false; }
        } else {
            std::string path = g_plugins + "\\" + f.name;
            if (!Exists(path)) WriteFileAtomic(path, data[f.name]);
            else { int n = MergeIni(path, data[f.name]); if (n > 0) added += n; }
        }
    }
    { std::lock_guard<std::mutex> l(g_mx); g_lock[m.id] = m.version; SaveLock(); ScanInstalled(); }
    msg = m.id + " " + m.version + u8" установлен" + (added ? u8", в настройки добавлено новых параметров: " + std::to_string(added) : "");
    return true;
}
static void RunWorker(std::function<void()> fn) {
    if (g_busy.exchange(true)) return;
    std::thread([fn] { fn(); g_busy = false; }).detach();
}
static bool SetEnabled(Mod &m, bool on, std::string &msg) {   // caller holds g_mx
    if (GameRunning()) { msg = u8"Закройте игру, чтобы включать и выключать моды"; return false; }
    std::string dll = DllOf(m), a = g_plugins + "\\" + dll, b = g_disabled + "\\" + dll;
    CreateDirectoryW(W(g_disabled).c_str(), nullptr);
    BOOL ok = on ? MoveFileExW(W(b).c_str(), W(a).c_str(), MOVEFILE_REPLACE_EXISTING) : MoveFileExW(W(a).c_str(), W(b).c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) { msg = dll + u8": не удалось переместить"; return false; }
    m.enabled = on; msg = m.id + (on ? u8" включён" : u8" выключен (DLL в Mods\\Plugins\\disabled)");
    return true;
}
static bool RemoveMod(Mod &m, std::string &msg) {             // caller holds g_mx; settings and logs are kept
    if (GameRunning()) { msg = u8"Закройте игру перед удалением"; return false; }
    std::string dll = DllOf(m);
    DeleteFileW(W(g_plugins + "\\" + dll).c_str()); DeleteFileW(W(g_disabled + "\\" + dll).c_str());
    g_lock.erase(m.id); SaveLock(); ScanInstalled();
    msg = m.id + u8" удалён (настройки и логи оставлены)";
    return true;
}
static bool ModLoaderOk() { return Exists(g_gameDir + "\\winmm.dll") && Exists(g_gameDir + "\\EDFModLoader.dll"); }
static void Play() {
    std::string exe = g_gameExe.find(':') != std::string::npos ? g_gameExe : g_gameDir + "\\" + g_gameExe;
    std::wstring cmd = L"\"" + W(exe) + L"\" " + W(g_gameArgs);
    STARTUPINFOW si{ sizeof si }; PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, W(g_gameDir).c_str(), &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess); Status(u8"Игра запущена");
        if (g_closeOnPlay) PostQuitMessage(0);
    } else Status(u8"Не удалось запустить " + exe + " (" + std::to_string(GetLastError()) + ")");
}

// ---- launcher settings (EDF6Launcher.ini next to the exe) -------------------------------------------------
static void LoadConfig() {
    g_cfgPath = ExeDir() + "\\EDF6Launcher.ini";
    if (!Exists(g_cfgPath))
        WriteFileAtomic(g_cfgPath, "[Launcher]\r\n; game folder; empty = the folder of EDF6Launcher.exe\r\nGameDir=\r\n"
                                   "; program started by the Play button (relative to the game folder) and its arguments\r\nGameExe=EDF6.exe\r\nGameArgs=\r\n"
                                   "; 1 = close the launcher after starting the game\r\nCloseOnPlay=0\r\n"
                                   "; where the mod list comes from\r\nManifest=" + std::string(DEFAULT_MANIFEST) + "\r\n");
    auto get = [&](const wchar_t *k, const char *def) {
        wchar_t b[1024]; GetPrivateProfileStringW(L"Launcher", k, W(def).c_str(), b, 1024, W(g_cfgPath).c_str()); return U(b);
    };
    if (g_gameDir.empty()) g_gameDir = get(L"GameDir", "");
    if (g_gameDir.empty()) g_gameDir = ExeDir();
    g_gameExe = get(L"GameExe", "EDF6.exe"); g_gameArgs = get(L"GameArgs", "");
    g_closeOnPlay = get(L"CloseOnPlay", "0") == "1";
    g_plugins = g_gameDir + "\\Mods\\Plugins"; g_disabled = g_plugins + "\\disabled"; g_lockPath = g_plugins + "\\edf6mods.lock.json";
}

// ---- UI: themes --------------------------------------------------------------------------------------------
// One modern theme (plain window) and three "old PC game launcher" themes: a painted backdrop drawn in code
// (night city, giant ants on the roofs, a mothership with a beam, searchlights, tracers), a chunky title,
// slanted metal-plate buttons going down the screen, a "NEW!" starburst and a news ticker.
// All art is procedural: no game assets or third-party pictures are used.
struct Theme {
    const char *name, *about; bool retro;
    ImVec4 bg, panel, text, dim, accent, accent2, bad, button, buttonHover, frame;
    ImU32 sky0, sky1, horizon, city, window, beam, ant, plate0, plate1;
    float scanlines;              // 0..1
};
#define C4(r, g, b, a) ImVec4((r) / 255.f, (g) / 255.f, (b) / 255.f, (a))
static Theme g_themes[] = {
    { u8"Олдскул 2004", u8"Сумерки, хром и оранжевый неон — как лаунчеры с дисков из нулевых", true,
      C4(9, 11, 26, 1), C4(16, 20, 42, 1), C4(240, 244, 255, 1), C4(150, 165, 200, 1), C4(255, 158, 31, 1), C4(115, 190, 255, 1), C4(255, 90, 70, 1),
      C4(40, 48, 72, 1), C4(66, 78, 112, 1), C4(26, 32, 58, 1),
      IM_COL32(6, 8, 34, 255), IM_COL32(70, 30, 96, 255), IM_COL32(255, 122, 40, 255), IM_COL32(8, 8, 18, 255), IM_COL32(255, 206, 120, 255),
      IM_COL32(170, 215, 255, 55), IM_COL32(4, 4, 10, 255), IM_COL32(170, 180, 200, 255), IM_COL32(70, 78, 96, 255), 0.18f },
};
static const int THEME_COUNT = (int)(sizeof g_themes / sizeof g_themes[0]);
static const Theme &TH() { return g_themes[g_theme]; }

static ImFont *g_bold, *g_mid, *g_big;
static std::unique_ptr<IniFile> g_edit; static std::string g_editTitle; static int g_captureLine = -1;
enum Panel { P_NONE, P_MODS, P_SETTINGS };
static Panel g_panel = P_NONE;

static void ApplyTheme(int t) {
    g_theme = std::max(0, std::min(THEME_COUNT - 1, t));
    const Theme &T = TH();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = T.retro ? 2.f : 0.f; st.FrameRounding = T.retro ? 2.f : 3.f; st.FramePadding = ImVec2(8, 5);
    st.ItemSpacing = ImVec2(8, 6); st.CellPadding = ImVec2(8, 6); st.WindowBorderSize = T.retro ? 2.0f : 1.0f;
    ImVec4 *c = st.Colors;
    c[ImGuiCol_WindowBg] = T.bg; c[ImGuiCol_ChildBg] = T.panel; c[ImGuiCol_PopupBg] = ImVec4(T.panel.x, T.panel.y, T.panel.z, 0.98f);
    c[ImGuiCol_Border] = T.retro ? T.accent : T.frame; c[ImGuiCol_Text] = T.text; c[ImGuiCol_TextDisabled] = T.dim;
    c[ImGuiCol_FrameBg] = T.frame; c[ImGuiCol_FrameBgHovered] = T.button; c[ImGuiCol_FrameBgActive] = T.buttonHover;
    c[ImGuiCol_Button] = T.button; c[ImGuiCol_ButtonHovered] = T.buttonHover;
    c[ImGuiCol_ButtonActive] = ImVec4(T.accent.x, T.accent.y, T.accent.z, 0.6f);
    c[ImGuiCol_CheckMark] = T.accent; c[ImGuiCol_Header] = T.button; c[ImGuiCol_HeaderHovered] = T.buttonHover; c[ImGuiCol_HeaderActive] = T.buttonHover;
    c[ImGuiCol_TableHeaderBg] = T.frame; c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TableBorderLight] = T.frame; c[ImGuiCol_TableBorderStrong] = T.button;
    c[ImGuiCol_TitleBg] = T.panel; c[ImGuiCol_TitleBgActive] = T.button; c[ImGuiCol_Separator] = T.button;
    c[ImGuiCol_ScrollbarBg] = T.panel; c[ImGuiCol_ScrollbarGrab] = T.button; c[ImGuiCol_ScrollbarGrabHovered] = T.buttonHover;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
}
static ImU32 A(ImU32 c, float a) { return (c & 0x00FFFFFF) | ((ImU32)(std::max(0.f, std::min(1.f, a)) * ((c >> 24) & 0xFF)) << 24); }
static ImU32 Mix(ImU32 a, ImU32 b, float t) {
    auto ch = [&](int s) { return (ImU32)((((a >> s) & 0xFF) * (1 - t) + ((b >> s) & 0xFF) * t)) << s; };
    return ch(0) | ch(8) | ch(16) | ch(24);
}
static ImU32 U32(const ImVec4 &v, float a = 1) { return ImGui::ColorConvertFloat4ToU32(ImVec4(v.x, v.y, v.z, v.w * a)); }
static uint32_t g_rng;
static float Rnd() { g_rng = g_rng * 1664525u + 1013904223u; return (g_rng >> 8) / 16777216.0f; }
static void OutlinedText(ImDrawList *dl, ImFont *f, float size, ImVec2 p, ImU32 col, ImU32 outline, const char *s, float th = 2) {
    for (int dx = -1; dx <= 1; ++dx) for (int dy = -1; dy <= 1; ++dy) if (dx || dy) dl->AddText(f, size, ImVec2(p.x + dx * th, p.y + dy * th), outline, s);
    dl->AddText(f, size, p, col, s);
}
// a giant ant in profile: abdomen, thorax, head, six legs, antennae
static void DrawAnt(ImDrawList *dl, ImVec2 c, float s, ImU32 col, ImU32 rim, float t, bool faceLeft) {
    float d = faceLeft ? -1.f : 1.f;
    ImVec2 abd(c.x - d * 1.15f * s, c.y - 0.05f * s), tho(c.x, c.y - 0.15f * s), head(c.x + d * 0.95f * s, c.y - 0.35f * s);
    for (int i = 0; i < 3; ++i) {                     // legs, three pairs, slowly stepping
        float bx = c.x + d * (-0.25f + i * 0.3f) * s, ph = sinf(t * 2.2f + i * 2.1f) * 0.12f * s;
        ImVec2 k(bx + d * (0.2f * s + ph), c.y - 0.55f * s), f(bx + d * (0.55f * s + ph), c.y + 0.75f * s);
        ImVec2 k2(bx - d * (0.1f * s - ph), c.y - 0.45f * s), f2(bx - d * (0.35f * s - ph), c.y + 0.75f * s);
        float o = std::max(2.0f, 0.035f * s);
        dl->AddLine(ImVec2(bx, c.y - 0.1f * s), k, rim, 0.11f * s + o); dl->AddLine(k, f, rim, 0.08f * s + o);
        dl->AddLine(ImVec2(bx, c.y - 0.1f * s), k2, rim, 0.10f * s + o); dl->AddLine(k2, f2, rim, 0.07f * s + o);
        dl->AddLine(ImVec2(bx, c.y - 0.1f * s), k, col, 0.11f * s); dl->AddLine(k, f, col, 0.08f * s);
        dl->AddLine(ImVec2(bx, c.y - 0.1f * s), k2, col, 0.10f * s); dl->AddLine(k2, f2, col, 0.07f * s);
    }
    float ro = std::max(2.0f, 0.035f * s);
    dl->AddEllipseFilled(abd, ImVec2(0.75f * s + ro, 0.48f * s + ro), rim, d * 0.25f);
    dl->AddEllipseFilled(tho, ImVec2(0.42f * s + ro, 0.26f * s + ro), rim, -d * 0.2f);
    dl->AddCircleFilled(head, 0.3f * s + ro, rim);
    dl->AddEllipseFilled(abd, ImVec2(0.75f * s, 0.48f * s), col, d * 0.25f);
    dl->AddEllipse(abd, ImVec2(0.75f * s, 0.48f * s), rim, d * 0.25f, 0, 0.04f * s);
    dl->AddEllipseFilled(tho, ImVec2(0.42f * s, 0.26f * s), col, -d * 0.2f);
    dl->AddCircleFilled(head, 0.3f * s, col);
    dl->AddCircle(head, 0.3f * s, rim, 0, 0.04f * s);
    float aw = sinf(t * 3.1f) * 0.08f * s;
    dl->AddBezierQuadratic(head, ImVec2(head.x + d * 0.3f * s, head.y - 0.7f * s), ImVec2(head.x + d * (0.75f * s + aw), head.y - 0.55f * s), col, 0.05f * s);
    dl->AddBezierQuadratic(head, ImVec2(head.x + d * 0.5f * s, head.y - 0.6f * s), ImVec2(head.x + d * (0.95f * s - aw), head.y - 0.25f * s), col, 0.05f * s);
    dl->AddCircleFilled(ImVec2(head.x + d * 0.12f * s, head.y - 0.05f * s), 0.06f * s, rim);   // eye glint
}

// ---- the backdrop -------------------------------------------------------------------------------------------
static void DrawArt(ImDrawList *dl, ImVec2 p0, ImVec2 p1, const Theme &T, float t) {
    float Wd = p1.x - p0.x, Ht = p1.y - p0.y;
    auto P = [&](float x, float y) { return ImVec2(p0.x + x * Wd, p0.y + y * Ht); };
    dl->AddRectFilledMultiColor(p0, p1, T.sky0, T.sky0, T.sky1, T.sky1);
    // stars
    g_rng = 7;
    for (int i = 0; i < 140; ++i) {
        float x = Rnd(), y = Rnd() * 0.55f, s = 0.6f + Rnd() * 1.4f, tw = 0.5f + 0.5f * sinf(t * (1.5f + Rnd() * 2) + i);
        dl->AddCircleFilled(P(x, y), s, A(IM_COL32(255, 255, 255, 255), 0.25f + 0.6f * tw * (1 - y)));
    }
    // planet with halo
    ImVec2 moon = P(0.16f, 0.24f); float mr = 0.085f * Ht;
    for (int i = 6; i > 0; --i) dl->AddCircleFilled(moon, mr * (1 + i * 0.22f), A(T.horizon, 0.035f));
    dl->AddCircleFilled(moon, mr, Mix(T.window, IM_COL32(255, 255, 255, 255), 0.4f));
    dl->AddCircleFilled(ImVec2(moon.x + mr * 0.35f, moon.y - mr * 0.15f), mr * 0.85f, A(T.sky0, 0.55f));
    // the mothership: drifts slowly, blinking rim lights, tractor beam
    ImVec2 ship = P(0.50f + sinf(t * 0.11f) * 0.03f, 0.17f + sinf(t * 0.37f) * 0.006f);
    float sw = 0.20f * Wd, sh = 0.045f * Ht;
    ImVec2 beamTop1(ship.x - sw * 0.18f, ship.y + sh * 0.6f), beamTop2(ship.x + sw * 0.18f, ship.y + sh * 0.6f);
    float bp = 0.5f + 0.5f * sinf(t * 1.3f);
    dl->AddQuadFilled(beamTop1, beamTop2, P(0.62f, 0.86f), P(0.40f, 0.86f), A(T.beam, 0.5f + 0.5f * bp));
    dl->AddEllipseFilled(ImVec2(ship.x, ship.y - sh * 0.5f), ImVec2(sw * 0.35f, sh * 0.9f), Mix(T.city, T.plate1, 0.5f));
    dl->AddEllipseFilled(ship, ImVec2(sw * 0.5f, sh), Mix(T.city, T.plate1, 0.35f));
    dl->AddEllipse(ship, ImVec2(sw * 0.5f, sh), A(T.horizon, 0.6f), 0, 0, 2.0f);
    for (int i = 0; i < 14; ++i) {
        float a = -3.14159f + i * 3.14159f / 13, on = sinf(t * 4 + i * 0.9f) > 0.2f ? 1.f : 0.25f;
        dl->AddCircleFilled(ImVec2(ship.x + cosf(a) * sw * 0.44f, ship.y + sinf(-a) * sh * -0.55f + sh * 0.2f), 2.6f, A(T.window, on));
    }
    // horizon glow and the burning skyline
    dl->AddRectFilledMultiColor(P(0, 0.55f), p1, A(T.horizon, 0), A(T.horizon, 0), A(T.horizon, 0.55f), A(T.horizon, 0.55f));
    g_rng = 1234;
    float x = -0.02f; std::vector<std::pair<ImVec2, ImVec2>> roofs;
    while (x < 1.02f) {
        float w = 0.03f + Rnd() * 0.06f, h = 0.10f + Rnd() * 0.30f;
        if (x > 0.58f && x < 0.70f) h *= 0.55f;                   // a gap under the beam
        ImVec2 a = P(x, 1 - h), b = P(x + w, 1);
        dl->AddRectFilled(a, b, T.city);
        dl->AddLine(a, ImVec2(b.x, a.y), A(T.horizon, 0.35f), 1.5f);
        for (float wy = a.y + 8; wy < b.y - 6; wy += 11)          // windows, some flicker
            for (float wx = a.x + 5; wx < b.x - 5; wx += 9)
                if (Rnd() < 0.33f) dl->AddRectFilled(ImVec2(wx, wy), ImVec2(wx + 4, wy + 5), A(T.window, (Rnd() < 0.1f ? 0.5f + 0.5f * sinf(t * 7 + wx) : 0.75f) * 0.8f));
        roofs.push_back({ a, ImVec2(b.x, a.y) });
        x += w + Rnd() * 0.01f;
    }
    // searchlights sweeping the sky
    for (int i = 0; i < 3; ++i) {
        ImVec2 base = P(0.12f + i * 0.33f, 1.0f);
        float ang = -1.57f + sinf(t * (0.35f + i * 0.13f) + i * 2) * 0.55f, len = Ht * 1.1f, spread = 0.06f;
        dl->AddTriangleFilled(base, ImVec2(base.x + cosf(ang - spread) * len, base.y + sinf(ang - spread) * len),
                              ImVec2(base.x + cosf(ang + spread) * len, base.y + sinf(ang + spread) * len), A(T.beam, 0.55f));
    }
    // tracers fired at the ship
    for (int i = 0; i < 6; ++i) {
        float ph = fmodf(t * 0.9f + i * 0.37f, 1.f);
        ImVec2 from = P(0.05f + i * 0.17f, 0.98f), to(ship.x + (i - 3) * 20.f, ship.y + sh);
        ImVec2 a(from.x + (to.x - from.x) * ph, from.y + (to.y - from.y) * ph), b(from.x + (to.x - from.x) * (ph + 0.05f), from.y + (to.y - from.y) * (ph + 0.05f));
        dl->AddLine(a, b, A(T.window, 1 - ph), 2.0f);
    }
    // ants on the roofs and a huge one in front
    if (roofs.size() > 8) {
        auto onRoof = [&](size_t k, float s, bool left) { ImVec2 r = roofs[k].first; DrawAnt(dl, ImVec2((r.x + roofs[k].second.x) / 2, r.y - s * 0.75f), s, T.ant, A(T.horizon, 0.7f), t + k, left); };
        onRoof(2, 0.030f * Ht, false); onRoof(roofs.size() - 4, 0.026f * Ht, true); onRoof(roofs.size() / 2 + 1, 0.022f * Ht, true);
    }
    DrawAnt(dl, P(0.20f, 0.90f), 0.11f * Ht, T.ant, A(T.horizon, 0.9f), t * 0.6f, false);
    // CRT scanlines and a vignette
    if (T.scanlines > 0) for (float y = p0.y; y < p1.y; y += 3) dl->AddLine(ImVec2(p0.x, y), ImVec2(p1.x, y), A(IM_COL32(0, 0, 0, 255), T.scanlines * 0.45f));
    float v = 0.22f * Wd;
    dl->AddRectFilledMultiColor(p0, ImVec2(p0.x + v, p1.y), IM_COL32(0, 0, 0, 150), 0, 0, IM_COL32(0, 0, 0, 150));
    dl->AddRectFilledMultiColor(ImVec2(p1.x - v, p0.y), p1, 0, IM_COL32(0, 0, 0, 120), IM_COL32(0, 0, 0, 120), 0);
}

// slanted metal plate button with rivets; hazard stripes and a glow when hovered
static bool PlateButton(const char *id, const char *label, ImVec2 pos, ImVec2 size, bool primary, bool enabled = true) {
    const Theme &T = TH();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float skew = size.y * 0.45f;
    ImGui::SetCursorScreenPos(pos);
    bool clicked = ImGui::InvisibleButton(id, ImVec2(size.x + skew, size.y)) && enabled;
    bool hov = ImGui::IsItemHovered() && enabled, act = ImGui::IsItemActive() && enabled;
    ImVec2 o(pos.x + (act ? 3.f : 0.f), pos.y + (act ? 3.f : 0.f));
    ImVec2 q[4] = { ImVec2(o.x + skew, o.y), ImVec2(o.x + size.x + skew, o.y), ImVec2(o.x + size.x, o.y + size.y), ImVec2(o.x, o.y + size.y) };
    float t = (float)ImGui::GetTime();
    // shadow
    dl->AddQuadFilled(ImVec2(q[0].x + 6, q[0].y + 7), ImVec2(q[1].x + 6, q[1].y + 7), ImVec2(q[2].x + 6, q[2].y + 7), ImVec2(q[3].x + 6, q[3].y + 7), IM_COL32(0, 0, 0, 140));
    ImU32 top = primary ? U32(T.accent) : T.plate0, bot = primary ? Mix(U32(T.accent), IM_COL32(0, 0, 0, 255), 0.55f) : T.plate1;
    if (!enabled) { top = Mix(top, IM_COL32(60, 60, 60, 255), 0.7f); bot = Mix(bot, IM_COL32(30, 30, 30, 255), 0.7f); }
    if (hov) { top = Mix(top, IM_COL32(255, 255, 255, 255), 0.18f); }
    // metal gradient: split the parallelogram into upper and lower halves
    ImVec2 ml(o.x + skew * 0.5f, o.y + size.y * 0.5f), mr(o.x + size.x + skew * 0.5f, o.y + size.y * 0.5f);
    dl->AddQuadFilled(q[0], q[1], mr, ml, top);
    dl->AddQuadFilled(ml, mr, q[2], q[3], bot);
    if (hov) {                                               // hazard stripes run along the plate
        dl->PushClipRect(ImVec2(o.x, o.y), ImVec2(o.x + size.x + skew, o.y + size.y), true);
        for (float sx = o.x - 60 + fmodf(t * 60, 30); sx < o.x + size.x + skew; sx += 30)
            dl->AddQuadFilled(ImVec2(sx, o.y + size.y), ImVec2(sx + 12, o.y + size.y), ImVec2(sx + 12 + skew, o.y), ImVec2(sx + skew, o.y), A(IM_COL32(0, 0, 0, 255), 0.18f));
        dl->PopClipRect();
    }
    dl->AddPolyline(q, 4, hov ? U32(T.accent) : A(IM_COL32(255, 255, 255, 255), 0.35f), ImDrawFlags_Closed, hov ? 3.f : 1.5f);
    dl->AddLine(q[0], q[1], A(IM_COL32(255, 255, 255, 255), 0.5f), 1.5f);           // top bevel highlight
    for (int i = 0; i < 4; ++i) {                                                    // rivets
        ImVec2 r((q[i].x + (i < 2 ? 0 : 0)) + (i == 0 || i == 3 ? 12.f : -12.f), q[i].y + (i < 2 ? 9.f : -9.f));
        dl->AddCircleFilled(r, 3.2f, A(IM_COL32(0, 0, 0, 255), 0.45f)); dl->AddCircleFilled(ImVec2(r.x - 0.8f, r.y - 0.8f), 1.8f, A(IM_COL32(255, 255, 255, 255), 0.55f));
    }
    ImFont *f = primary ? g_big : g_mid; float fs = primary ? size.y * 0.62f : size.y * 0.52f;
    ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, label);
    ImVec2 tp(o.x + skew * 0.5f + (size.x - ts.x) * 0.5f, o.y + (size.y - ts.y) * 0.5f);
    if (hov) OutlinedText(dl, f, fs, ImVec2(tp.x - 28 - fabsf(sinf(t * 6)) * 6, tp.y), U32(T.accent), IM_COL32(0, 0, 0, 255), u8"»", 1.5f);
    OutlinedText(dl, f, fs, tp, enabled ? (primary ? IM_COL32(20, 14, 4, 255) : U32(T.text)) : IM_COL32(150, 150, 150, 255),
                 primary ? A(IM_COL32(255, 255, 255, 255), 0.35f) : IM_COL32(0, 0, 0, 200), label, primary ? 1.0f : 2.0f);
    return clicked;
}
// rotating starburst sticker ("NEW!") in the old-launcher spirit
static void Starburst(ImVec2 c, float r, const char *line1, const char *line2) {
    ImDrawList *dl = ImGui::GetWindowDrawList(); const Theme &T = TH();
    float t = (float)ImGui::GetTime(), rot = t * 0.4f, pulse = 1 + 0.06f * sinf(t * 5);
    ImVec2 pts[32];
    for (int i = 0; i < 32; ++i) { float a = rot + i * 6.2832f / 32, rr = (i & 1 ? 0.72f : 1.0f) * r * pulse; pts[i] = ImVec2(c.x + cosf(a) * rr, c.y + sinf(a) * rr); }
    for (int i = 0; i < 32; ++i) dl->AddTriangleFilled(c, pts[i], pts[(i + 1) % 32], U32(T.accent2));   // a star is not convex: fan from the centre
    dl->AddPolyline(pts, 32, IM_COL32(255, 255, 255, 200), ImDrawFlags_Closed, 2);
    float fs = r * 0.36f;
    ImVec2 s1 = g_mid->CalcTextSizeA(fs, FLT_MAX, 0, line1), s2 = g_mid->CalcTextSizeA(fs * 0.8f, FLT_MAX, 0, line2);
    OutlinedText(dl, g_mid, fs, ImVec2(c.x - s1.x / 2, c.y - s1.y * 0.95f), IM_COL32(255, 255, 255, 255), IM_COL32(0, 0, 0, 220), line1, 1.5f);
    OutlinedText(dl, g_mid, fs * 0.8f, ImVec2(c.x - s2.x / 2, c.y + 1), IM_COL32(255, 255, 255, 255), IM_COL32(0, 0, 0, 220), line2, 1.5f);
}

// ---- ini editor (shared by all themes) -------------------------------------------------------------------
static std::string KeyName(int vk) {
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC); wchar_t b[64] = {};
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (sc && GetKeyNameTextW((LONG)(sc << 16), b, 64)) return U(b);
    char h[16]; sprintf(h, "0x%02X", vk); return h;
}
static bool IsBoolLine(const IniLine &L) {
    return (L.value == "0" || L.value == "1") && (L.hint.find("1 =") != std::string::npos || L.hint.find("0 =") != std::string::npos);
}
static bool IsKeyLine(const IniLine &L) { return L.key.size() > 3 && L.key.compare(L.key.size() - 3, 3, "Key") == 0; }
static void DrawIniEditor() {
    if (!g_edit) return;
    ImGuiIO &io = ImGui::GetIO(); const Theme &T = TH();
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.82f, io.DisplaySize.y * 0.82f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowFocus();
    bool open = true;
    std::string title = u8"Настройки: " + g_editTitle + (g_edit->dirty ? " *" : "") + "###ini";
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(T.dim, "%s", g_edit->path.c_str());
        ImGui::BeginChild("lines", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.4f), true);
        if (ImGui::BeginTable("ini", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, 230); ImGui::TableSetupColumn("v");
            for (int i = 0; i < (int)g_edit->lines.size(); ++i) {
                IniLine &L = g_edit->lines[i];
                if (!L.isKey) continue;
                ImGui::PushID(i); ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::Text("%s", L.key.c_str());
                if (!L.hint.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", L.hint.c_str());
                ImGui::TableNextColumn();
                if (IsBoolLine(L)) {
                    bool b = L.value == "1";
                    if (ImGui::Checkbox("##b", &b)) { L.value = b ? "1" : "0"; g_edit->dirty = true; }
                } else if (IsKeyLine(L) && (L.value.rfind("0x", 0) == 0 || L.value.empty())) {
                    int vk = (int)strtol(L.value.c_str(), nullptr, 0);
                    std::string label = g_captureLine == i ? u8"нажмите клавишу... (Esc — отмена)" : (vk ? KeyName(vk) + "  (" + L.value + ")" : u8"не задана");
                    if (ImGui::Button(label.c_str(), ImVec2(260, 0))) g_captureLine = i;
                } else {
                    char buf[512]; strncpy(buf, L.value.c_str(), sizeof buf - 1); buf[sizeof buf - 1] = 0;
                    ImGui::SetNextItemWidth(-1);
                    if (ImGui::InputText("##v", buf, sizeof buf)) { L.value = buf; g_edit->dirty = true; }
                }
                if (!L.hint.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(T.dim, "%s", L.hint.c_str()); ImGui::PopTextWrapPos(); }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        if (ImGui::Button(u8"Сохранить")) { if (SaveIni(*g_edit)) Status(u8"Настройки сохранены: " + g_edit->path); else Status(u8"Не удалось сохранить настройки"); }
        ImGui::SameLine(); if (ImGui::Button(u8"Открыть в Блокноте")) ShellExecuteW(nullptr, L"open", L"notepad.exe", W("\"" + g_edit->path + "\"").c_str(), nullptr, SW_SHOWNORMAL);
        ImGui::SameLine(); if (ImGui::Button(u8"Перечитать")) { std::string p = g_edit->path; LoadIni(p, *g_edit); }
        ImGui::SameLine(); ImGui::TextColored(T.dim, u8"Наведите на имя параметра — подсказка. Изменения действуют после перезапуска игры.");
    }
    ImGui::End();
    if (g_captureLine >= 0) {                                    // the next key pressed becomes the value
        for (int vk = 8; vk < 256; ++vk) {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;
            if (GetAsyncKeyState(vk) & 1) {
                if (vk != VK_ESCAPE) { char h[16]; sprintf(h, "0x%02X", vk); g_edit->lines[g_captureLine].value = h; g_edit->dirty = true; }
                g_captureLine = -1; break;
            }
        }
    }
    if (!open) { g_edit.reset(); g_captureLine = -1; }
}
static void OpenIni(const Mod &m) {
    std::string p = g_plugins + "\\" + IniOf(m);
    auto ini = std::make_unique<IniFile>();
    if (!LoadIni(p, *ini)) { g_status = u8"Нет файла настроек " + p + u8" — установите мод"; return; }
    g_edit = std::move(ini); g_editTitle = m.id;
}
static void OpenLauncherIni() { auto ini = std::make_unique<IniFile>(); if (LoadIni(g_cfgPath, *ini)) { g_edit = std::move(ini); g_editTitle = "EDF6 Launcher"; } }
static bool AnyUpdate() { for (auto &m : g_mods) if (m.inManifest && m.present && m.installed != m.version) return true; return false; }
static int MissingCount() { int n = 0; for (auto &m : g_mods) if (m.inManifest && !m.present) ++n; return n; }

// ---- shared pieces (caller holds g_mx) -----------------------------------------------------------------------
static void DrawModsTable(bool busy) {
    const Theme &T = TH();
    if (ImGui::BeginTable("mods", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(u8"Вкл", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn(u8"Мод");
        ImGui::TableSetupColumn(u8"Установлен", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn(u8"На GitHub", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 380);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < g_mods.size(); ++i) {
            Mod &m = g_mods[i];
            ImGui::PushID((int)i); ImGui::TableNextRow(); ImGui::TableNextColumn();
            bool en = m.enabled;
            ImGui::BeginDisabled(!m.present || busy);
            if (ImGui::Checkbox("##on", &en)) { std::string msg; SetEnabled(m, en, msg); g_status = msg; }
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            ImGui::PushFont(g_bold); ImGui::Text("%s", m.id.c_str()); ImGui::PopFont();
            if (!m.inManifest) { ImGui::SameLine(); ImGui::TextColored(T.dim, u8"(не из репозитория)"); }
            if (!m.description.empty()) { ImGui::PushTextWrapPos(); ImGui::TextColored(T.dim, "%s", m.description.c_str()); ImGui::PopTextWrapPos(); }
            ImGui::TableNextColumn();
            if (!m.present) ImGui::TextColored(T.dim, u8"нет");
            else if (m.installed == "?") { ImGui::Text(u8"есть"); if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"Установлен вручную, версия неизвестна"); }
            else ImGui::Text("%s", m.installed.c_str());
            ImGui::TableNextColumn();
            bool upd = m.inManifest && m.present && m.installed != m.version;
            if (!m.inManifest) ImGui::TextColored(T.dim, "-");
            else if (!m.present) ImGui::Text("%s", m.version.c_str());
            else if (upd) ImGui::TextColored(T.accent, u8"%s ↑", m.version.c_str());
            else ImGui::TextColored(T.accent2, u8"%s ✓", m.version.c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(busy);
            if (m.inManifest && (!m.present || upd)) {
                if (ImGui::Button(m.present ? u8"Обновить" : u8"Установить", ImVec2(110, 0))) {
                    Mod copy = m;
                    RunWorker([copy] { std::string msg; InstallMod(copy, msg); Status(msg); });
                }
            } else ImGui::Dummy(ImVec2(110, 0));
            ImGui::SameLine(); if (ImGui::Button(u8"Настройки")) OpenIni(m);
            ImGui::SameLine(); if (ImGui::Button(u8"Лог")) { std::string p = g_plugins + "\\" + (m.log.empty() ? m.id + ".log" : m.log); if (Exists(p)) OpenPath(p); else g_status = u8"Лога пока нет: " + p; }
            if (!m.report.empty()) { ImGui::SameLine(); if (ImGui::Button(u8"Отчёт")) { std::string p = g_plugins + "\\" + m.report; if (Exists(p)) OpenPath(p); else g_status = u8"Отчёта пока нет — сыграйте миссию"; } }
            if (m.present) { ImGui::SameLine(); if (ImGui::Button(u8"Удалить")) ImGui::OpenPopup("del"); }
            if (ImGui::BeginPopup("del")) {
                ImGui::Text(u8"Удалить %s? Настройки и логи останутся.", m.id.c_str());
                if (ImGui::Button(u8"Удалить")) { std::string msg; RemoveMod(m, msg); g_status = msg; ImGui::CloseCurrentPopup(); }
                ImGui::SameLine(); if (ImGui::Button(u8"Отмена")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (g_mods.empty()) ImGui::TextColored(T.dim, u8"Список модов пуст: %s", g_manifestOk ? "" : g_manifestErr.c_str());
}
static void UpdateButtons(bool busy) {
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(u8"Проверить обновления")) RunWorker([] { Status(u8"Проверяю..."); LoadManifest(); });
    ImGui::SameLine();
    ImGui::BeginDisabled(!AnyUpdate());
    if (ImGui::Button(u8"Обновить всё")) {
        std::vector<Mod> todo; for (auto &m : g_mods) if (m.inManifest && m.present && m.installed != m.version) todo.push_back(m);
        RunWorker([todo] { std::string all; for (auto &m : todo) { std::string msg; InstallMod(m, msg); all += msg + "; "; } Status(all); });
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine(); if (ImGui::Button(u8"Папка модов")) OpenPath(g_plugins);
}
static void ModLoaderLine() {
    const Theme &T = TH();
    if (ModLoaderOk()) ImGui::TextColored(T.accent2, u8"EDFModLoader установлен");
    else {
        ImGui::TextColored(T.bad, u8"EDFModLoader не найден (нет winmm.dll или EDFModLoader.dll) — моды не загрузятся");
        ImGui::SameLine(); if (ImGui::SmallButton(u8"Скачать EDFModLoader")) OpenPath(g_modloaderPage);
    }
    if (!Exists(g_gameDir + "\\" + g_gameExe) && g_gameExe.find(':') == std::string::npos)
        ImGui::TextColored(T.bad, u8"В папке игры нет %s — укажите GameDir/GameExe в настройках лаунчера", g_gameExe.c_str());
}

// ---- retro layout --------------------------------------------------------------------------------------------
static void DrawRetro(bool busy) {
    ImGuiIO &io = ImGui::GetIO(); const Theme &T = TH();
    float t = (float)ImGui::GetTime(), Wd = io.DisplaySize.x, Ht = io.DisplaySize.y;
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("retro", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    DrawArt(dl, ImVec2(0, 0), io.DisplaySize, T, t);

    // title block, top left, a little crooked on purpose
    float big = Ht * 0.105f;
    OutlinedText(dl, g_big, big * 0.62f, ImVec2(Wd * 0.045f, Ht * 0.05f), U32(T.text), IM_COL32(0, 0, 0, 255), "EARTH DEFENSE FORCE", 3);
    OutlinedText(dl, g_big, big * 2.0f, ImVec2(Wd * 0.045f + big * 0.1f, Ht * 0.05f + big * 0.48f), U32(T.accent), IM_COL32(0, 0, 0, 255), "6", 4);
    OutlinedText(dl, g_mid, big * 0.42f, ImVec2(Wd * 0.045f + big * 1.25f, Ht * 0.05f + big * 0.95f), U32(T.accent2), IM_COL32(0, 0, 0, 255), u8"Л А У Н Ч Е Р   М О Д О В", 2);
    OutlinedText(dl, g_bold, big * 0.24f, ImVec2(Wd * 0.045f + big * 1.27f, Ht * 0.05f + big * 1.45f), A(IM_COL32(255, 255, 255, 255), 0.8f), IM_COL32(0, 0, 0, 200),
                 u8"Земля сама себя не защитит. Сначала — моды.", 1.5f);

    // the plates: a diagonal stack on the right
    struct B { const char *label; int action; };
    bool running = GameRunning();
    B buttons[] = { { running ? u8"ИГРА ИДЁТ" : u8"ИГРАТЬ", 0 }, { u8"МОДЫ", 1 }, { u8"НАСТРОЙКИ", 2 }, { u8"ОТЧЁТ", 3 }, { u8"ВЫХОД", 4 } };
    float bw = Wd * 0.30f, bh = Ht * 0.085f, x0 = Wd * 0.60f, y0 = Ht * 0.24f;
    ImGui::BeginDisabled(g_panel != P_NONE || g_edit != nullptr);
    for (int i = 0; i < 5; ++i) {
        float bx = x0 + i * Wd * 0.018f + (i == 0 ? -Wd * 0.03f : 0), by = y0 + i * (bh * 1.22f) + (i ? bh * 0.35f : 0);
        ImVec2 sz = i == 0 ? ImVec2(bw * 1.12f, bh * 1.35f) : ImVec2(bw, bh);
        if (PlateButton(buttons[i].label, buttons[i].label, ImVec2(bx, by), sz, i == 0, !(i == 0 && running))) {
            switch (buttons[i].action) {
            case 0: Play(); break;
            case 1: g_panel = P_MODS; break;
            case 2: g_panel = P_SETTINGS; break;
            case 3: { std::string p = g_plugins + "\\EDFStats\\latest.html"; if (Exists(p)) OpenPath(p); else g_status = u8"Отчёта пока нет — сыграйте миссию с EDFStats"; } break;
            case 4: PostQuitMessage(0); break;
            }
        }
    }
    ImGui::EndDisabled();
    // stickers
    if (!ModLoaderOk()) Starburst(ImVec2(x0 - Wd * 0.05f, y0 + bh * 2.1f), Ht * 0.075f, u8"НЕТ", u8"ЗАГРУЗЧИКА!");
    else if (AnyUpdate()) Starburst(ImVec2(x0 - Wd * 0.04f, y0 + bh * 2.1f), Ht * 0.075f, u8"НОВОЕ!", u8"обновления");
    else if (int n = MissingCount()) { std::string s = "+" + std::to_string(n); Starburst(ImVec2(x0 - Wd * 0.04f, y0 + bh * 2.1f), Ht * 0.07f, s.c_str(), n == 1 ? u8"новый мод" : u8"новых мода"); }

    // installed mods as dog tags, bottom left
    float ty = Ht * 0.62f;
    OutlinedText(dl, g_mid, Ht * 0.032f, ImVec2(Wd * 0.045f, ty), U32(T.accent), IM_COL32(0, 0, 0, 255), u8"В СТРОЮ:", 2);
    ty += Ht * 0.045f;
    for (auto &m : g_mods) {
        if (!m.present) continue;
        std::string s = std::string(m.enabled ? u8"■ " : u8"□ ") + m.id + (m.installed != "?" && !m.installed.empty() ? "  v" + m.installed : "");
        OutlinedText(dl, g_bold, Ht * 0.026f, ImVec2(Wd * 0.05f, ty), m.enabled ? U32(T.text) : U32(T.dim), IM_COL32(0, 0, 0, 220), s.c_str(), 1.5f);
        ty += Ht * 0.034f;
    }

    // news ticker along the bottom
    float th = Ht * 0.05f, tyy = Ht - th;
    dl->AddRectFilled(ImVec2(0, tyy), ImVec2(Wd, tyy + th), A(U32(T.bg), 0.85f));
    dl->AddLine(ImVec2(0, tyy), ImVec2(Wd, tyy), U32(T.accent), 2);
    dl->AddRectFilled(ImVec2(0, tyy), ImVec2(Wd * 0.13f, tyy + th), U32(T.accent));
    OutlinedText(dl, g_mid, th * 0.55f, ImVec2(Wd * 0.012f, tyy + th * 0.2f), IM_COL32(20, 14, 4, 255), A(IM_COL32(255, 255, 255, 255), 0.3f), u8"СВОДКА", 1);
    std::string news = g_status + u8"   •   ";
    for (auto &m : g_mods) if (m.inManifest) news += m.id + " " + m.version + u8"   •   ";
    news += u8"Гигантские муравьи замечены в секторе 7   •   F6 в бою — панель урона, F7 — таблица   •   Не стреляйте по своим, EDFStats всё видит   •   ";
    news += u8"Папка игры: " + g_gameDir + u8"   •   ";
    ImVec2 ns = g_bold->CalcTextSizeA(th * 0.5f, FLT_MAX, 0, news.c_str());
    float off = fmodf(t * 70.f, ns.x);
    dl->PushClipRect(ImVec2(Wd * 0.13f, tyy), ImVec2(Wd, tyy + th), true);
    for (float sx = Wd * 0.14f - off; sx < Wd; sx += ns.x) dl->AddText(g_bold, th * 0.5f, ImVec2(sx, tyy + th * 0.24f), U32(T.text), news.c_str());
    dl->PopClipRect();
    ImGui::SetCursorScreenPos(ImVec2(Wd - 110, 12));
    ImGui::TextColored(T.dim, u8"версия %s", LAUNCHER_VERSION);
    ImGui::End();

    // panels over the art
    if (g_panel != P_NONE) {
        ImGui::SetNextWindowSize(ImVec2(Wd * (g_panel == P_MODS ? 0.86f : 0.6f), g_panel == P_MODS ? Ht * 0.74f : Ht * 0.34f), ImGuiCond_Always);
        ImGui::SetNextWindowPos(ImVec2(Wd * 0.5f, Ht * 0.47f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        bool open = true;
        const char *title = g_panel == P_MODS ? u8"АРСЕНАЛ МОДОВ###panel" : u8"НАСТРОЙКИ###panel";
        ImGui::PushFont(g_bold);
        if (ImGui::Begin(title, &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
            ImGui::PopFont();
            if (g_panel == P_MODS) {
                ModLoaderLine();
                ImGui::BeginChild("mods", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.3f), true);
                DrawModsTable(busy);
                ImGui::EndChild();
                UpdateButtons(busy);
            } else {
                ImGui::PushFont(g_bold); ImGui::Text(u8"Игра"); ImGui::PopFont();
                ImGui::TextColored(T.dim, u8"Папка: %s", g_gameDir.c_str());
                ImGui::TextColored(T.dim, u8"Запуск: %s %s", g_gameExe.c_str(), g_gameArgs.c_str());
                ModLoaderLine();
                if (ImGui::Button(u8"Настройки лаунчера (папка, exe, аргументы)")) OpenLauncherIni();
                ImGui::SameLine(); if (ImGui::Button(u8"Папка игры")) OpenPath(g_gameDir);
            }
        } else ImGui::PopFont();
        ImGui::End();
        if (!open) g_panel = P_NONE;
    }
}

static void DrawRoot() {
    bool busy = g_busy;
    std::lock_guard<std::mutex> lock(g_mx);
    DrawRetro(busy);
}

// ---- window + D3D11 (based on the Dear ImGui win32/dx11 example) ---------------------------------------
static ID3D11Device *g_dev; static ID3D11DeviceContext *g_ctx; static IDXGISwapChain *g_sc; static ID3D11RenderTargetView *g_rtv;
static UINT g_resizeW, g_resizeH;
static void MakeRtv() { ID3D11Texture2D *bb; g_sc->GetBuffer(0, IID_PPV_ARGS(&bb)); g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); }
static LRESULT WINAPI WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return true;
    if (msg == WM_SIZE && wp != SIZE_MINIMIZED) { g_resizeW = LOWORD(lp); g_resizeH = HIWORD(lp); return 0; }
    if (msg == WM_GETMINMAXINFO) { ((MINMAXINFO *)lp)->ptMinTrackSize = { 900, 520 }; return 0; }
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}
static void SaveBmp(const std::string &path) {
    ID3D11Texture2D *bb; g_sc->GetBuffer(0, IID_PPV_ARGS(&bb));
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d); d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    ID3D11Texture2D *st; g_dev->CreateTexture2D(&d, nullptr, &st); g_ctx->CopyResource(st, bb);
    D3D11_MAPPED_SUBRESOURCE m; g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m);
    FILE *f = _wfopen(W(path).c_str(), L"wb");
    BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{}; ih.biSize = sizeof ih; ih.biWidth = d.Width; ih.biHeight = -(LONG)d.Height; ih.biPlanes = 1; ih.biBitCount = 32;
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
    fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f);
    std::vector<uint8_t> row(d.Width * 4);
    for (UINT y = 0; y < d.Height; ++y) {
        const uint8_t *s = (const uint8_t *)m.pData + y * m.RowPitch;
        for (UINT x = 0; x < d.Width; ++x) { row[x * 4] = s[x * 4 + 2]; row[x * 4 + 1] = s[x * 4 + 1]; row[x * 4 + 2] = s[x * 4]; row[x * 4 + 3] = 255; }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f); g_ctx->Unmap(st, 0); st->Release(); bb->Release();
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc; wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::string shot; bool selftest = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--game-dir" && i + 1 < argc) g_gameDir = U(argv[++i]);
        else if (a == L"--manifest" && i + 1 < argc) g_manifestSrc = U(argv[++i]);
        else if (a == L"--screenshot" && i + 1 < argc) shot = U(argv[++i]);
        else if (a == L"--selftest-install") selftest = true;
    }
    LoadConfig();
    if (selftest) {                    // install every mod from the list into --game-dir and report (no window)
        AttachConsole(ATTACH_PARENT_PROCESS); FILE *o = _wfopen(W(ExeDir() + "\\selftest.txt").c_str(), L"w");
        LoadManifest();
        std::vector<Mod> list; { std::lock_guard<std::mutex> l(g_mx); list = g_mods; }
        fprintf(o, "manifest: %s %s, %zu mods\n", g_manifestOk ? "ok" : "FAILED", g_manifestErr.c_str(), list.size());
        for (auto &m : list) { std::string msg; bool ok = InstallMod(m, msg); fprintf(o, "%s %s: %s\n", ok ? "OK  " : "FAIL", m.id.c_str(), msg.c_str()); }
        { std::lock_guard<std::mutex> l(g_mx); for (auto &m : g_mods) fprintf(o, "state %s installed=%s present=%d enabled=%d\n", m.id.c_str(), m.installed.c_str(), m.present, m.enabled); }
        fclose(o); return 0;
    }

    WNDCLASSEXW wc{ sizeof wc, CS_CLASSDC, WndProc, 0, 0, inst, LoadIconW(nullptr, IDI_APPLICATION), LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, L"EDF6Launcher", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"EDF6 Launcher", WS_OVERLAPPEDWINDOW, 80, 60, 1280, 760, nullptr, nullptr, inst, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, nullptr, &g_ctx)) &&
        FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &g_sc, &g_dev, nullptr, &g_ctx))) {
        MessageBoxW(nullptr, L"Direct3D 11 is not available", L"EDF6 Launcher", MB_ICONERROR); return 1;
    }
    MakeRtv();
    if (shot.empty()) { ShowWindow(hwnd, show); UpdateWindow(hwnd); }

    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO(); io.IniFilename = nullptr;
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x0400, 0x052F, 0x2000, 0x206F, 0x2190, 0x21FF, 0x25A0, 0x25FF, 0x2600, 0x26FF, 0x2700, 0x27BF, 0 };
    wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH);
    std::string fonts = U(win) + "\\Fonts\\";
    if (!Exists(fonts + "segoeui.ttf") || !io.Fonts->AddFontFromFileTTF((fonts + "segoeui.ttf").c_str(), 18.0f, nullptr, ranges)) io.Fonts->AddFontDefault();
    if (Exists(fonts + "seguisym.ttf")) { ImFontConfig mc; mc.MergeMode = true; io.Fonts->AddFontFromFileTTF((fonts + "seguisym.ttf").c_str(), 18.0f, &mc, ranges); }
    g_bold = Exists(fonts + "segoeuib.ttf") ? io.Fonts->AddFontFromFileTTF((fonts + "segoeuib.ttf").c_str(), 22.0f, nullptr, ranges) : io.Fonts->Fonts[0];
    // chunky poster font for the retro themes (Impact has Cyrillic); Segoe UI Bold if it is missing
    std::string poster = Exists(fonts + "impact.ttf") ? fonts + "impact.ttf" : fonts + "segoeuib.ttf";
    g_mid = Exists(poster) ? io.Fonts->AddFontFromFileTTF(poster.c_str(), 34.0f, nullptr, ranges) : g_bold;
    g_big = Exists(poster) ? io.Fonts->AddFontFromFileTTF(poster.c_str(), 96.0f, nullptr, ranges) : g_bold;
    ApplyTheme(g_theme);
    ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_dev, g_ctx);

    if (shot.empty()) RunWorker([] { Status(u8"Загружаю список модов..."); LoadManifest(); });
    else { LoadManifest(); }

    int frames = 0;
    for (bool done = false; !done;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); if (msg.message == WM_QUIT) done = true; }
        if (done) break;
        if (g_resizeW) { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } g_sc->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0); g_resizeW = g_resizeH = 0; MakeRtv(); }
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        DrawRoot(); DrawIniEditor();
        ImGui::Render();
        const float clear[4] = { 0.05f, 0.07f, 0.06f, 1 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr); g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (!shot.empty() && ++frames == 4) {
            auto frame = [&] {
                ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame(); DrawRoot(); DrawIniEditor(); ImGui::Render();
                g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr); g_ctx->ClearRenderTargetView(g_rtv, clear); ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            };
            std::string base = shot.substr(0, shot.size() - 4);
            for (int k = 0; k < 3; ++k) frame(); SaveBmp(base + "_main.bmp");
            g_panel = P_MODS; for (int k = 0; k < 3; ++k) frame(); SaveBmp(base + "_mods.bmp");
            g_panel = P_SETTINGS; for (int k = 0; k < 3; ++k) frame(); SaveBmp(base + "_settings.bmp");
            break;
        }
        g_sc->Present(1, 0);
    }
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release(); g_sc->Release(); g_ctx->Release(); g_dev->Release();
    DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}
