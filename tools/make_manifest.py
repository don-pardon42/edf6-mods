"""Builds mods.json (the list the launcher reads) from the files that go into the releases.

usage: python tools/make_manifest.py
Reads _release/<Mod>/* (the same files uploaded to the GitHub release of each mod) and writes mods.json
in the repository root with versions, download links and SHA-256 of every file.
Edit MODS below when a mod gets a new version, then upload the files to the release with the same tag.
"""
import hashlib, json, os

REPO = "don-pardon42/edf6-mods"
HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODS = [
    {"id": "EDFStats", "version": "0.9", "tag": "edfstats-v0.9",
     "description": "Статистика игроков в кооперативе: урон, DPS, убийства, урон по своим, поднятия. Окно в игре (F6/F7) и HTML-отчёт после миссии.",
     "description_en": "Co-op player statistics: damage, DPS, kills, friendly fire, rescues. In-game overlay (F6/F7) and an HTML report after each mission.",
     "files": ["EDFStats.dll", "EDFStats.ini"], "log": "EDFStats.log", "report": "EDFStats\\latest.html"},
    {"id": "SpectateCam", "version": "0.4", "tag": "spectatecam-v0.4",
     "description": "После смерти камера следит за живыми союзниками: = / - игроки, 9 NPC, 0 к себе.",
     "description_en": "After you die the camera follows living allies: = / - players, 9 NPCs, 0 back to you.",
     "files": ["SpectateCam.dll", "SpectateCam.ini"], "log": "SpectateCam.log"},
    {"id": "SquadSync", "version": "0.1", "tag": "squadsync-v0.1",
     "description": "Подтягивает персонажей других игроков к их настоящей позиции, когда сетевая синхронизация отстаёт. Нужны IP игроков в настройках.",
     "description_en": "Pulls other players' characters to their real position when the game's network sync lags behind. Needs the players' IPs in the settings.",
     "files": ["SquadSync.dll", "SquadSync.ini"], "log": "SquadSync.log"},
]

def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()

out = {"schema": 1, "repo": REPO,
       "modloader": {"name": "EDFModLoader", "page": "https://github.com/BlueAmulet/EDFModLoader/releases"},
       "mods": []}
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
