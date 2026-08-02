@echo off
REM Clear MSYS/MINGW env vars to bypass IDF MSys detection when called from Git Bash
set MSYSTEM=
set MSYSTEM_CHOST=
set MSYSTEM_PREFIX=
set MINGW_CHOST=
set MINGW_PREFIX=
set MINGW_PACKAGE_PREFIX=
set CHERE_INVOKING=
set MSYS2_PATH_TYPE=

REM Toolchain installed at C:\Espressif (see .vscode/settings.json idf.toolsPath)
set IDF_TOOLS_PATH=C:\Espressif

call "F:\software\.espressif\v6.0\esp-idf\export.bat"
if errorlevel 1 (
    echo EXPORT_FAILED
    exit /b 1
)

cd /d "G:\Robot_v2\DCU\ESP32_COMM"
idf.py build
exit /b %errorlevel%