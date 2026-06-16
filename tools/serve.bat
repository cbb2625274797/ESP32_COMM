@echo off
:: ESP32 COMM Host Server Launcher (Windows)
:: Double-click to start, then visit http://localhost:8080

set PORT=8080

echo ========================================
echo   ESP32 COMM Host Server
echo   PORT: %PORT%
echo ========================================
echo.
echo   OTA Console:   http://localhost:%PORT%/index.html
echo.
echo   Switch WiFi / BLE mode in the "Device Connect" card.
echo.
echo   Press Ctrl+C to stop
echo ========================================
echo.

cd /d "%~dp0"
python -m http.server %PORT%
pause
