@echo off
rem Builds MortarCam.dll (x64, static CRT) into MortarCam\out.
setlocal
cd /d "%~dp0"
for /f "delims=" %%i in ('"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath') do set "VS=%%i"
if not defined VS (echo Visual Studio with C++ tools not found & exit /b 1)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set D=..\deps
if not exist obj mkdir obj
if not exist out mkdir out
cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 /LD /W3 /I%D% /I%D%\minhook\include /I%D%\minhook\src /Foobj\ ^
   src\mortarcam.cpp %D%\minhook\src\buffer.c %D%\minhook\src\hook.c %D%\minhook\src\trampoline.c %D%\minhook\src\hde\hde64.c ^
   /Fe:out\MortarCam.dll /link /DLL user32.lib || exit /b 1
copy /y src\MortarCam.ini out\MortarCam.ini >nul
echo Built: MortarCam\out\MortarCam.dll
