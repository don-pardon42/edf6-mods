// ---- launcher self-update ---------------------------------------------------------------------------------------
// (included from launcher.cpp after logs.inc)
// mods.json carries "launcher": {"version", "url", "sha256"}. When that version is newer than this exe, the settings and
// mods panels offer "Update the launcher": the new exe is downloaded and checked (SHA-256), this exe is renamed to
// EDF6Launcher.exe.old (Windows allows renaming a running exe), the new one takes its name and is started, and this one
// quits. The next start deletes the .old file.

// "0.4.0" < "0.4.1" < "0.10": numbers compared one by one
static int CompareVersions(const std::string &a, const std::string &b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        long x = 0, y = 0;
        while (i < a.size() && isdigit((unsigned char)a[i])) x = x * 10 + (a[i++] - '0');
        while (j < b.size() && isdigit((unsigned char)b[j])) y = y * 10 + (b[j++] - '0');
        if (x != y) return x < y ? -1 : 1;
        while (i < a.size() && !isdigit((unsigned char)a[i])) ++i;
        while (j < b.size() && !isdigit((unsigned char)b[j])) ++j;
    }
    return 0;
}
static bool LauncherUpdateAvailable() {            // caller holds g_mx
    return !g_launcherLatest.empty() && !g_launcherUrl.empty() && CompareVersions(LAUNCHER_VERSION, g_launcherLatest) < 0;
}
static std::string SelfPath() { wchar_t b[MAX_PATH]; GetModuleFileNameW(nullptr, b, MAX_PATH); return U(b); }
static void CleanupOldLauncher() {                   // the previous exe may still be closing: try for a few seconds
    std::string old = SelfPath() + ".old";
    if (!Exists(old)) return;
    std::thread([old] { for (int i = 0; i < 20 && Exists(old); ++i) { DeleteFileW(W(old).c_str()); Sleep(500); } }).detach();
}
// downloads, checks and swaps the exe; restart = start the new exe and quit. Returns the status line.
static std::string UpdateLauncher(bool restart) {
    std::string url, sha, ver;
    { std::lock_guard<std::mutex> l(g_mx); url = g_launcherUrl; sha = g_launcherSha; ver = g_launcherLatest; }
    if (url.empty()) return L_(u8"В списке модов нет новой версии лаунчера", u8"The mod list has no launcher update");
    Status(L_(u8"Скачиваю лаунчер ", u8"Downloading launcher ") + ver + "...");
    std::string body, err;
    bool got = url.rfind("http", 0) == 0 ? HttpGet(url, body, err) : (ReadFile(url, body) || ((err = "file not found"), false));
    if (!got) return L_(u8"Не удалось скачать лаунчер: ", u8"Cannot download the launcher: ") + err;
    if (sha.empty() || Sha256Hex(body) != sha) return L_(u8"Контрольная сумма лаунчера не совпала, обновление отменено", u8"Launcher checksum mismatch, update cancelled");
    if (body.size() < 2 || body[0] != 'M' || body[1] != 'Z') return L_(u8"Скачанный файл — не программа, обновление отменено", u8"The download is not a program, update cancelled");
    std::string self = SelfPath(), fresh = self + ".new", old = self + ".old";
    if (!WriteFileAtomic(fresh, body)) return L_(u8"Не удалось записать новый лаунчер рядом со старым", u8"Cannot write the new launcher next to the old one");
    DeleteFileW(W(old).c_str());
    if (!MoveFileExW(W(self).c_str(), W(old).c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(W(fresh).c_str()); return L_(u8"Не удалось переименовать текущий лаунчер", u8"Cannot rename the running launcher"); }
    if (!MoveFileExW(W(fresh).c_str(), W(self).c_str(), MOVEFILE_REPLACE_EXISTING)) {
        MoveFileExW(W(old).c_str(), W(self).c_str(), MOVEFILE_REPLACE_EXISTING);      // put the old one back
        return L_(u8"Не удалось поставить новый лаунчер, оставлен старый", u8"Cannot put the new launcher in place, the old one is kept");
    }
    if (restart) {
        std::wstring cmd = L"\"" + W(self) + L"\"";
        if (g_gameDir != ExeDir()) cmd += L" --game-dir \"" + W(g_gameDir) + L"\"";
        STARTUPINFOW si{ sizeof si }; PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, W(ExeDir()).c_str(), &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess); g_quitRequested = true;   // the UI thread closes the window
        } else return L_(u8"Лаунчер обновлён до ", u8"Launcher updated to ") + ver + L_(u8" — запустите его заново", u8" - start it again");
    }
    return L_(u8"Лаунчер обновлён до ", u8"Launcher updated to ") + ver;
}
// UI (called with g_mx held)
static void LauncherUpdateButton(bool busy) {
    if (!LauncherUpdateAvailable()) return;
    std::string label = std::string(L_(u8"Обновить лаунчер до ", u8"Update the launcher to ")) + g_launcherLatest;
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(label.c_str())) RunWorker([] { std::string s = UpdateLauncher(true); Status(s); });
    ImGui::EndDisabled();
}
