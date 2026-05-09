@echo off
setlocal

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
set "BUILD_DIR=%ROOT%\build"
set "QT_DIR=C:\Qt\6.6.3\msvc2019_64"
set "EXE=%BUILD_DIR%\Release\spotify-recorder-qt.exe"
set "LOG=%TEMP%\spotifystretch-debug.log"

if exist "%LOG%" del /q "%LOG%"

if not exist "%QT_DIR%\bin\windeployqt.exe" (
  echo Qt not found at "%QT_DIR%".
  echo Update QT_DIR in this script to match your install.
  exit /b 1
)

cmake -S "%ROOT%" -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH="%QT_DIR%"
if errorlevel 1 exit /b %errorlevel%

cmake --build "%BUILD_DIR%" --config Release --verbose
if errorlevel 1 exit /b %errorlevel%

"%QT_DIR%\bin\windeployqt.exe" "%EXE%"
if errorlevel 1 exit /b %errorlevel%

if not exist "%EXE%" (
  echo Build finished but "%EXE%" was not created.
  exit /b 1
)

start "" "%EXE%"
