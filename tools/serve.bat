@echo off
:: ESP32 COMM Host Server Launcher (Windows)
:: Double-click to start, then visit http://localhost:8080

set PORT=8080

echo ========================================
echo   ESP32 COMM Host Server
echo   PORT: %PORT%
echo ========================================
echo.
echo   WiFi OTA:   http://localhost:%PORT%/index.html
echo   BLE  OTA:   http://localhost:%PORT%/ble_ota.html
echo.
echo   Press Ctrl+C to stop
echo ========================================
echo.

cd /d "%~dp0"
python -m http.server %PORT%
pause
