# EDF6 Launcher (черновик 0.1)

Маленький лаунчер для модов EDF6: один `EDF6Launcher.exe` (~0.8 МБ, ничего ставить не нужно), кладётся в папку игры.

- **Моды** — список из [`mods.json`](../mods.json) этого репозитория: установка, обновление, удаление,
  включение/выключение (выключенный мод = DLL в `Mods\Plugins\disabled`, EDFModLoader подпапки не загружает).
  Моды, которых нет в списке, тоже показываются — их можно включать, выключать и настраивать.
- **Обновление** — файлы качаются с GitHub Releases, проверяется SHA-256; старая DLL сохраняется как `.bak`.
  Ваши `.ini` не затираются: дописываются только новые параметры.
- **Настройки** — редактор `.ini` любого мода: комментарии становятся подсказками, 0/1 — галочками,
  клавиши (`...Key`) задаются нажатием.
- **Играть** — запускает игру (`GameExe` в `EDF6Launcher.ini`, по умолчанию `EDF6.exe`).
- Проверяет, что EDFModLoader установлен, и не даёт обновлять моды при запущенной игре.

Настройки самого лаунчера — `EDF6Launcher.ini` рядом с exe (создаётся при первом запуске):
`GameDir`, `GameExe`, `GameArgs`, `CloseOnPlay`, `Manifest`.

## Сборка и проверка

```bat
Launcher\build.bat
Launcher\out\EDF6Launcher.exe --game-dir C:\temp\fakegame --selftest-install   & rem ставит все моды в тестовую папку, отчёт в out\selftest.txt
Launcher\out\EDF6Launcher.exe --screenshot out\shot.bmp                         & rem рисует окно в картинку
```

## Выпуск новой версии мода

1. Положить файлы мода в `_release\<Мод>\`, поднять версию в `tools\make_manifest.py`.
2. `gh release create <мод>-v<версия> _release\<Мод>\*.dll _release\<Мод>\*.ini`
3. `python tools\make_manifest.py`, закоммитить и отправить `mods.json` — у всех лаунчер предложит обновиться.
