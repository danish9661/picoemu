#!/bin/bash

echo "==================================================="
echo "   OpenHW Studio - Private IoT Gateway (Go)"
echo "==================================================="
echo ""

if [ -f "openhw-gw" ]; then
    echo "[INFO] Found pre-compiled gateway."
else
    echo "[INFO] Compiling Gateway (First Time Setup)..."
    
    if command -v go &> /dev/null; then
        echo "[INFO] Compiling using local Go installation..."
        go mod tidy
        go build -o openhw-gw .
    elif command -v docker &> /dev/null; then
        echo "[INFO] Go is not installed, but Docker is!"
        echo "[INFO] Compiling using Docker..."
        docker run --rm -v "$(pwd):/app" -w /app golang:1.23-alpine sh -c "go mod tidy && GOOS=linux GOARCH=amd64 go build -o openhw-gw ."
    else
        echo "[ERROR] You need either 'Go' or 'Docker' installed to compile the gateway for the first time."
        exit 1
    fi
fi

echo ""
echo "[SUCCESS] Starting the Private Gateway..."
echo ""
echo "==================================================="
echo "Keep this window open while using the simulator!"
echo "Press Ctrl+C to stop the gateway."
echo "==================================================="
echo ""

./openhw-gw
