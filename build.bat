@echo off
rem Build ExtendedNotepad. No args: app only. "test": also build+run selftest.
setlocal
set VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat
if not exist "%VCVARS%" (
  echo vcvars64.bat not found: "%VCVARS%"
  exit /b 1
)
call "%VCVARS%" >nul
if not exist build mkdir build

rc /nologo /c65001 /fo build\app.res app.rc
if errorlevel 1 exit /b 1

cl /nologo /utf-8 /O2 /W3 /EHsc /std:c++17 /DUNICODE /D_UNICODE ^
   /Fo:build\ /Fe:build\ExtendedNotepad.exe main.cpp build\app.res ^
   /link /SUBSYSTEM:WINDOWS
if errorlevel 1 exit /b 1

if /i "%1"=="test" (
  cl /nologo /utf-8 /Od /W3 /EHsc /std:c++17 /DUNICODE /D_UNICODE /DSELFTEST ^
     /Fo:build\t_ /Fe:build\selftest.exe main.cpp /link /SUBSYSTEM:CONSOLE
  if errorlevel 1 exit /b 1
  build\selftest.exe || exit /b 1
)
echo built: build\ExtendedNotepad.exe
