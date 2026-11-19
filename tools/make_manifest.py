"""Builds mods.json (the list the launcher reads) from the files that go into the releases.

usage: python tools/make_manifest.py
Reads _release/<Mod>/* (the same files uploaded to the GitHub release of each mod) and writes mods.json
in the repository root with versions, download links and SHA-256 of every file.
Edit MODS below when a mod gets a new version, then upload the files to the release with the same tag.
LAUNCHER is the launcher's own entry (its "update the launcher" button): _release/Launcher/EDF6Launcher.exe,
uploaded to the release launcher-v<version>.
"""
import hashlib, json, os

REPO = "don-pardon42/edf6-mods"
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODS = [
    {"id": "EDFStats", "version": "0.15", "tag": "edfstats-v0.15",
     "description": "Статистика игроков в кооперативе: урон, DPS, убийства, урон по своим, урон и DPS по каждому оружию. Окно в игре (F6/F7) и HTML-отчёт после миссии.",
     "description_en": "Co-op player statistics: damage, DPS, kills, friendly fire, damage and DPS per weapon. In-game overlay (F6/F7) and an HTML report after each mission.",
     "files": ["EDFStats.dll", "EDFStats.ini"], "log": "EDFStats.log", "report": "EDFStats\\latest.html"},
    {"id": "SpectateCam", "version": "0.4", "tag": "spectatecam-v0.4",
     "description": "После смерти камера следит за живыми союзниками: = / - игроки, 9 NPC, 0 к себе.",
     "description_en": "After you die the camera follows living allies: = / - players, 9 NPCs, 0 back to you.",
     "files": ["SpectateCam.dll", "SpectateCam.ini"], "log": "SpectateCam.log"},
    {"id": "SquadSync", "version": "0.1", "tag": "squadsync-v0.1",
     "description": "Подтягивает персонажей других игроков к их настоящей позиции, когда сетевая синхронизация отстаёт. Нужны IP игроков в настройках.",
     "description_en": "Pulls other players' characters to their real position when the game's network sync lags behind. Needs the players' IPs in the settings.",
     "files": ["SquadSync.dll", "SquadSync.ini"], "log": "SquadSync.log"},
    {"id": "MortarCam", "version": "0.1.1", "tag": "mortarcam-v0.1.1",
     "description": "Фенсер: клавиша B поднимает камеру над полем боя, пока экипирована мортира (как вид бомбардировщика у Air Raider). Пробная версия: пишет подробный лог для доработки.",
     "description_en": "Fencer: the B key raises the camera above the battlefield while a mortar is equipped (like Air Raider's bomber view). Test version: writes a detailed log for further work.",
     "files": ["MortarCam.dll", "MortarCam.ini"], "log": "MortarCam.log"},
]

LAUNCHER = {"version": "0.4.0", "tag": "launcher-v0.4.0", "file": "EDF6Launcher.exe"}

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()

out = {"schema": 1, "repo": REPO,
       "modloader": {"name": "EDFModLoader", "page": "https://github.com/BlueAmulet/EDFModLoader/releases"},
       "mods": []}
_lexe = os.path.join(HERE, "_release", "Launcher", LAUNCHER["file"])
out["launcher"] = {"version": LAUNCHER["version"],
                   "url": f"https://github.com/{REPO}/releases/download/{LAUNCHER['tag']}/{LAUNCHER['file']}",
                   "sha256": sha256(_lexe)}
for m in MODS:
    files = []
    for name in m["files"]:
        path = os.path.join(HERE, "_release", m["id"], name)
        files.append({"name": name, "kind": "dll" if name.lower().endswith(".dll") else "ini",
                      "url": f"https://github.com/{REPO}/releases/download/{m['tag']}/{name}", "sha256": sha256(path)})
    entry = {k: v for k, v in m.items() if k not in ("files", "tag")}
    entry["files"] = files
    out["mods"].append(entry)
with open(os.path.join(HERE, "mods.json"), "w", encoding="utf-8", newline="\n") as f:
    json.dump(out, f, ensure_ascii=False, indent=2)
    f.write("\n")
print("mods.json written:", ", ".join(f"{m['id']} {m['version']}" for m in MODS))
