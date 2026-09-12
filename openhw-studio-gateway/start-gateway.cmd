@echo off
echo ===================================================
echo    OpenHW Studio - Private IoT Gateway (Go)
echo ===================================================
echo.

:: Check if the executable already exists
IF EXIST "openhw-gw.exe" (
    echo [INFO] Found pre-compiled gateway.
    goto :run_gateway
)

echo [INFO] Compiling Gateway (First Time Setup)...
:: Check if Go is installed locally
go version >nul 2>&1
IF %ERRORLEVEL% EQU 0 (
    echo [INFO] Compiling using local Go installation...
    go mod tidy
    go build -o openhw-gw.exe .
    goto :run_gateway
)

:: If Go is not installed, try using Docker
docker -v >nul 2>&1
IF %ERRORLEVEL% EQU 0 (
    echo [INFO] Go is not installed, but Docker is!
    echo [INFO] Compiling using Docker...
    docker run --rm -v "%cd%:/app" -w /app golang:1.23-alpine sh -c "go mod tidy && GOOS=windows GOARCH=amd64 go build -o openhw-gw.exe ."
    goto :run_gateway
)

:: Neither Go nor Docker
echo [ERROR] Go is not installed on your system!
echo To run the Private Gateway, you need to install Go.
set /p INSTALL_GO="Would you like to install Go automatically using the command line (winget)? (Y/N): "

if /i "%INSTALL_GO%"=="Y" goto :install_go
goto :skip_install

:install_go
echo [INFO] Installing Go... Please wait, an installer prompt may appear.
winget install GoLang.Go
echo.
echo [SUCCESS] Go has been installed!
echo IMPORTANT: You must close this window and open a new one for the installation to take effect.
echo After closing, double click start-gateway.cmd again.
pause
exit /b

:skip_install
echo You must install Go or Docker to compile the gateway.
pause
exit /b

:run_gateway
echo.
echo [SUCCESS] Starting the Private Gateway...
echo.
echo ===================================================
echo Keep this window open while using the simulator!
echo Press Ctrl+C to stop the gateway.
echo ===================================================
echo.

openhw-gw.exe
pause
