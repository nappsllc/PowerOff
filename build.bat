@echo off
rem Build PowerOff (C version) with MSVC. Produces poweroff.exe (~tens of KB).
setlocal
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b 1
cl /nologo /O1 /Os /GL /Gy /MD /DNDEBUG /W3 poweroff.c /link /OPT:REF /OPT:ICF /INCREMENTAL:NO /FILEALIGN:512 /SUBSYSTEM:CONSOLE /MANIFEST:NO /OUT:poweroff.exe
if errorlevel 1 exit /b 1
"C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64\mt.exe" -nologo -manifest poweroff.exe.manifest -outputresource:poweroff.exe
if errorlevel 1 exit /b 1
dir poweroff.exe
