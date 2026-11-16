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
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static const char *LAUNCHER_VERSION = "0.1";
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
    std::string man = get(L"Manifest", DEFAULT_MANIFEST); if (g_manifestSrc == DEFAULT_MANIFEST && !man.empty()) g_manifestSrc = man;
    g_plugins = g_gameDir + "\\Mods\\Plugins"; g_disabled = g_plugins + "\\disabled"; g_lockPath = g_plugins + "\\edf6mods.lock.json";
}

// ---- UI --------------------------------------------------------------------------------------------------
static const ImVec4 GOLD(0.89f, 0.73f, 0.23f, 1), GREEN(0.37f, 0.82f, 0.55f, 1), DIM(0.56f, 0.64f, 0.60f, 1), RED(0.88f, 0.42f, 0.33f, 1);
static ImFont *g_bold;
static std::unique_ptr<IniFile> g_edit; static std::string g_editTitle; static int g_captureLine = -1;

static void Style() {
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 0; st.FrameRounding = 3; st.FramePadding = ImVec2(8, 5); st.ItemSpacing = ImVec2(8, 6); st.CellPadding = ImVec2(8, 6);
    ImVec4 *c = st.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.05f, 0.07f, 0.06f, 1);
    c[ImGuiCol_ChildBg] = ImVec4(0.08f, 0.11f, 0.10f, 1);
    c[ImGuiCol_PopupBg] = ImVec4(0.07f, 0.10f, 0.09f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.24f, 0.32f, 0.28f, 1);
    c[ImGuiCol_Text] = ImVec4(0.91f, 0.94f, 0.91f, 1);
    c[ImGuiCol_TextDisabled] = DIM;
    c[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.17f, 0.15f, 1); c[ImGuiCol_FrameBgHovered] = ImVec4(0.17f, 0.23f, 0.20f, 1); c[ImGuiCol_FrameBgActive] = ImVec4(0.20f, 0.27f, 0.24f, 1);
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.20f, 0.17f, 1); c[ImGuiCol_ButtonHovered] = ImVec4(0.22f, 0.30f, 0.26f, 1); c[ImGuiCol_ButtonActive] = ImVec4(0.89f, 0.73f, 0.23f, 0.6f);
    c[ImGuiCol_CheckMark] = GOLD; c[ImGuiCol_Header] = ImVec4(0.14f, 0.20f, 0.17f, 1); c[ImGuiCol_HeaderHovered] = ImVec4(0.22f, 0.30f, 0.26f, 1);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.10f, 0.14f, 0.12f, 1); c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.17f, 0.23f, 0.20f, 1); c[ImGuiCol_TableBorderStrong] = ImVec4(0.24f, 0.32f, 0.28f, 1);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.14f, 0.12f, 1); c[ImGuiCol_TitleBg] = ImVec4(0.08f, 0.11f, 0.10f, 1);
    c[ImGuiCol_Separator] = ImVec4(0.24f, 0.32f, 0.28f, 1);
}
static std::string KeyName(int vk) {
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC); wchar_t b[64] = {};
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (sc && GetKeyNameTextW((LONG)(sc << 16), b, 64)) return U(b);
    char h[16]; sprintf(h, "0x%02X", vk); return h;
}
static bool IsBoolLine(const IniLine &L) {
    return (L.value == "0" || L.value == "1") && (L.hint.find("1 =") != std::string::npos || L.hint.find("0 =") != std::string::npos);
}
static bool IsKeyLine(const IniLine &L) {
    return L.key.size() > 3 && L.key.compare(L.key.size() - 3, 3, "Key") == 0;
}
static void DrawIniEditor() {
    if (!g_edit) return;
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.82f, io.DisplaySize.y * 0.82f), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    std::string title = u8"Настройки: " + g_editTitle + (g_edit->dirty ? " *" : "") + "###ini";
    if (ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextColored(DIM, "%s", g_edit->path.c_str());
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
                if (!L.hint.empty()) { ImGui::PushTextWrapPos(0); ImGui::TextColored(DIM, "%s", L.hint.c_str()); ImGui::PopTextWrapPos(); }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::EndChild();
        if (ImGui::Button(u8"Сохранить")) { if (SaveIni(*g_edit)) Status(u8"Настройки сохранены: " + g_edit->path); else Status(u8"Не удалось сохранить настройки"); }
        ImGui::SameLine(); if (ImGui::Button(u8"Открыть в Блокноте")) ShellExecuteW(nullptr, L"open", L"notepad.exe", W("\"" + g_edit->path + "\"").c_str(), nullptr, SW_SHOWNORMAL);
        ImGui::SameLine(); if (ImGui::Button(u8"Перечитать")) { std::string p = g_edit->path; LoadIni(p, *g_edit); }
        ImGui::SameLine(); ImGui::TextColored(DIM, u8"Наведите на имя параметра — подсказка. Изменения действуют после перезапуска игры.");
    }
    ImGui::End();
    // key capture: the next key pressed becomes the value
    if (g_captureLine >= 0) {
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
    if (!LoadIni(p, *ini)) { Status(u8"Нет файла настроек " + p + u8" — установите мод"); return; }
    g_edit = std::move(ini); g_editTitle = m.id;
}

static void DrawMain() {
    ImGuiIO &io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    bool busy = g_busy;
    std::lock_guard<std::mutex> lock(g_mx);

    ImGui::PushFont(g_bold); ImGui::TextColored(GOLD, "EDF6"); ImGui::SameLine(); ImGui::Text("LAUNCHER"); ImGui::PopFont();
    ImGui::SameLine(); ImGui::TextColored(DIM, u8"  версия %s", LAUNCHER_VERSION);
    ImGui::TextColored(DIM, u8"Папка игры: %s", g_gameDir.c_str());
    bool ml = ModLoaderOk();
    if (ml) ImGui::TextColored(GREEN, u8"EDFModLoader установлен");
    else {
        ImGui::TextColored(RED, u8"EDFModLoader не найден (нет winmm.dll или EDFModLoader.dll) — моды не загрузятся");
        ImGui::SameLine(); if (ImGui::SmallButton(u8"Скачать EDFModLoader")) OpenPath(g_modloaderPage);
    }
    if (!Exists(g_gameDir + "\\" + g_gameExe) && g_gameExe.find(':') == std::string::npos)
        ImGui::TextColored(RED, u8"В папке игры нет %s — укажите GameDir/GameExe в EDF6Launcher.ini", g_gameExe.c_str());
    ImGui::Spacing();

    ImGui::PushFont(g_bold); ImGui::Text(u8"МОДЫ"); ImGui::PopFont();
    float bottom = ImGui::GetFrameHeightWithSpacing() * 2.4f;
    ImGui::BeginChild("mods", ImVec2(0, -bottom), true);
    if (ImGui::BeginTable("mods", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(u8"Вкл", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn(u8"Мод");
        ImGui::TableSetupColumn(u8"Установлен", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn(u8"На GitHub", ImGuiTableColumnFlags_WidthFixed, 110);
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
            if (!m.inManifest) { ImGui::SameLine(); ImGui::TextColored(DIM, u8"(не из репозитория)"); }
            if (!m.description.empty()) { ImGui::PushTextWrapPos(); ImGui::TextColored(DIM, "%s", m.description.c_str()); ImGui::PopTextWrapPos(); }
            ImGui::TableNextColumn();
            if (!m.present) ImGui::TextColored(DIM, u8"нет");
            else if (m.installed == "?") { ImGui::Text(u8"есть"); if (ImGui::IsItemHovered()) ImGui::SetTooltip(u8"Установлен вручную, версия неизвестна"); }
            else ImGui::Text("%s", m.installed.c_str());
            ImGui::TableNextColumn();
            bool upd = m.inManifest && m.present && m.installed != m.version;
            if (!m.inManifest) ImGui::TextColored(DIM, "-");
            else if (!m.present) ImGui::Text("%s", m.version.c_str());
            else if (upd) ImGui::TextColored(GOLD, u8"%s ↑", m.version.c_str());
            else ImGui::TextColored(GREEN, u8"%s ✓", m.version.c_str());
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
    if (g_mods.empty()) ImGui::TextColored(DIM, u8"Список модов пуст: %s", g_manifestOk ? "" : g_manifestErr.c_str());
    ImGui::EndChild();

    // bottom bar
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(u8"Проверить обновления")) RunWorker([] { Status(u8"Проверяю..."); LoadManifest(); });
    ImGui::SameLine();
    bool anyUpd = false; for (auto &m : g_mods) anyUpd |= m.inManifest && m.present && m.installed != m.version;
    ImGui::BeginDisabled(!anyUpd);
    if (ImGui::Button(u8"Обновить всё")) {
        std::vector<Mod> todo; for (auto &m : g_mods) if (m.inManifest && m.present && m.installed != m.version) todo.push_back(m);
        RunWorker([todo] { std::string all; for (auto &m : todo) { std::string msg; InstallMod(m, msg); all += msg + "; "; } Status(all); });
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine(); if (ImGui::Button(u8"Папка модов")) OpenPath(g_plugins);
    ImGui::SameLine(); if (ImGui::Button(u8"Настройки лаунчера")) { auto ini = std::make_unique<IniFile>(); if (LoadIni(g_cfgPath, *ini)) { g_edit = std::move(ini); g_editTitle = "EDF6 Launcher"; } }
    float playW = 200;
    ImGui::SameLine(ImGui::GetWindowWidth() - playW - ImGui::GetStyle().WindowPadding.x);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.49f, 0.12f, 1)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.80f, 0.64f, 0.18f, 1));
    ImGui::PushFont(g_bold);
    bool running = GameRunning();
    ImGui::BeginDisabled(running);
    if (ImGui::Button(running ? u8"ИГРА ЗАПУЩЕНА" : u8"ИГРАТЬ", ImVec2(playW, ImGui::GetFrameHeight() * 1.5f))) Play();
    ImGui::EndDisabled();
    ImGui::PopFont(); ImGui::PopStyleColor(2);
    ImGui::TextColored(busy ? GOLD : DIM, "%s", g_status.c_str());
    ImGui::End();
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
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"EDF6 Launcher", WS_OVERLAPPEDWINDOW, 100, 100, 1120, 680, nullptr, nullptr, inst, nullptr);
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
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x0400, 0x052F, 0x2000, 0x206F, 0x2190, 0x21FF, 0x2700, 0x27BF, 0 };
    wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH);
    std::string fonts = U(win) + "\\Fonts\\";
    if (!Exists(fonts + "segoeui.ttf") || !io.Fonts->AddFontFromFileTTF((fonts + "segoeui.ttf").c_str(), 18.0f, nullptr, ranges)) io.Fonts->AddFontDefault();
    if (Exists(fonts + "seguisym.ttf")) { ImFontConfig mc; mc.MergeMode = true; io.Fonts->AddFontFromFileTTF((fonts + "seguisym.ttf").c_str(), 18.0f, &mc, ranges); }
    g_bold = Exists(fonts + "segoeuib.ttf") ? io.Fonts->AddFontFromFileTTF((fonts + "segoeuib.ttf").c_str(), 22.0f, nullptr, ranges) : io.Fonts->Fonts[0];
    Style();
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
        DrawMain(); DrawIniEditor();
        ImGui::Render();
        const float clear[4] = { 0.05f, 0.07f, 0.06f, 1 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr); g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (!shot.empty() && ++frames == 4) {
            SaveBmp(shot);
            if (!g_mods.empty()) {   // second picture: the settings editor of the first mod
                OpenIni(g_mods[0]);
                for (int k = 0; k < 3; ++k) {
                    ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame(); DrawMain(); DrawIniEditor(); ImGui::Render();
                    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr); g_ctx->ClearRenderTargetView(g_rtv, clear); ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
                }
                SaveBmp(shot.substr(0, shot.size() - 4) + "_settings.bmp");
            }
            break;
        }
        g_sc->Present(1, 0);
    }
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release(); g_sc->Release(); g_ctx->Release(); g_dev->Release();
    DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, inst);
    return 0;
}
