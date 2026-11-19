@echo off
rem Builds and runs the offline tests (no game needed): camera block blending, ownership, shell flight, ini parsing.
setlocal
cd /d "%~dp0"
for /f "delims=" %%i in ('"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath') do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if not exist obj mkdir obj
if not exist out mkdir out
cl /nologo /O2 /MT /EHsc /std:c++17 /utf-8 /W3 /I..\src /Foobj\ test_logic.cpp /Fe:out\test_logic.exe >nul || (echo BUILD FAILED test_logic & exit /b 1)
out\test_logic.exe
