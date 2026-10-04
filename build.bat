@echo off
rem Build PowerOff (C version) with MSVC. Produces poweroff.exe (~tens of KB).
rem Uses the current MSVC environment if cl.exe is on PATH (e.g. CI), else finds VS via vswhere.
setlocal
cd /d "%~dp0"
where cl >nul 2>nul
if errorlevel 1 (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if defined VSDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
where cl >nul 2>nul
if errorlevel 1 (echo cl.exe not found: install Visual Studio Build Tools with the C++ workload & exit /b 1)
cl /nologo /O1 /Os /GL /Gy /MD /DNDEBUG /W3 poweroff.c /link /OPT:REF /OPT:ICF /INCREMENTAL:NO /FILEALIGN:512 /SUBSYSTEM:WINDOWS /ENTRY:wmainCRTStartup /MANIFEST:NO /DELAYLOAD:comctl32.dll /DELAYLOAD:uxtheme.dll /DELAYLOAD:dwmapi.dll /DELAYLOAD:shell32.dll /DELAYLOAD:powrprof.dll delayimp.lib /OUT:poweroff.exe
if errorlevel 1 exit /b 1
mt.exe -nologo -manifest poweroff.exe.manifest -outputresource:poweroff.exe
if errorlevel 1 exit /b 1
dir poweroff.exe
