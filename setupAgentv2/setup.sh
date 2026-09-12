#!/usr/bin/env bash
# ==============================================================================
# Setup Agent v2 — Unified SSH & Bot Master Server
# ==============================================================================
set -e

PORT="${1:-1337}"
INSTALL_DIR="/opt/agentv2"

echo "=================================================="
echo "          Installing Agent v2 (Golang)            "
echo "=================================================="
echo "[*] Target Port: ${PORT}"
echo "[*] Install Path: ${INSTALL_DIR}"

# Check root privilege for system-wide service
IS_ROOT=0
if [ "$(id -u)" -eq 0 ]; then
    IS_ROOT=1
fi

mkdir -p "${INSTALL_DIR}"

# If running from repo
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PARENT_DIR="$(dirname "${SCRIPT_DIR}")"

if [ -f "${PARENT_DIR}/agentv2/agentv2" ]; then
    echo "[*] Copying pre-built agentv2 binary..."
    cp "${PARENT_DIR}/agentv2/agentv2" "${INSTALL_DIR}/agentv2"
elif [ -d "${PARENT_DIR}/agentv2" ] && command -v go >/dev/null 2>&1; then
    echo "[*] Building agentv2 from source..."
    (cd "${PARENT_DIR}/agentv2" && go build -ldflags="-s -w" -o "${INSTALL_DIR}/agentv2" ./cmd/agent)
else
    echo "[!] agentv2 binary not found. Please compile it first with 'go build ./cmd/agent'"
fi

# Copy configurations if exist
if [ -f "${PARENT_DIR}/users.json" ]; then
    cp -n "${PARENT_DIR}/users.json" "${INSTALL_DIR}/users.json" 2>/dev/null || true
fi
if [ -f "${PARENT_DIR}/methods.json" ]; then
    cp -n "${PARENT_DIR}/methods.json" "${INSTALL_DIR}/methods.json" 2>/dev/null || true
fi

chmod +x "${INSTALL_DIR}/agentv2" 2>/dev/null || true

# Setup systemd service if root & systemd available
if [ "${IS_ROOT}" -eq 1 ] && command -v systemctl >/dev/null 2>&1; then
    echo "[*] Creating systemd service (agentv2.service)..."
    cat <<EOF > /etc/systemd/system/agentv2.service
[Unit]
Description=Agent v2 Unified SSH & Master Server
After=network.target

[Service]
Type=simple
User=root
WorkingDirectory=${INSTALL_DIR}
ExecStart=${INSTALL_DIR}/agentv2 -p ${PORT}
Restart=always
RestartSec=3
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
EOF

    systemctl daemon-reload
    systemctl enable agentv2.service
    systemctl restart agentv2.service
    echo "[+] Service agentv2 started successfully via systemctl!"
else
    echo "[*] Non-root or no systemd. Creating background launcher script..."
    cat <<EOF > "${INSTALL_DIR}/start.sh"
#!/usr/bin/env bash
cd "${INSTALL_DIR}"
pkill -f "./agentv2" 2>/dev/null || true
nohup ./agentv2 -p ${PORT} > agentv2.log 2>&1 &
echo "[+] Agent v2 started in background (PID: \$!)."
EOF
    chmod +x "${INSTALL_DIR}/start.sh"
    echo "[+] You can start Agent v2 using: ${INSTALL_DIR}/start.sh"
fi

echo "=================================================="
echo "         Agent v2 Setup Complete!                 "
echo "=================================================="
