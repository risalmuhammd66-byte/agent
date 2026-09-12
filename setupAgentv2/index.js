/**
 * Agent v2 Setup Script - Pure Node.js (Pterodactyl & VPS Compatible)
 * Automatically detects Pterodactyl ports ($SERVER_PORT, $PORT, $BOT_PORT)
 * Runs pre-built agentv2 (Golang single-port server) & auto-downloads configs
 * Keeps container active and pipes logs directly to console
 */

const https = require('https');
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const REPO_BASE = 'https://raw.githubusercontent.com/risalmuhammd66-byte/agent/main';

const FILES_TO_DOWNLOAD = [
    { url: `${REPO_BASE}/methods.json`, filename: 'methods.json', executable: false },
    { url: `${REPO_BASE}/users.json`, filename: 'users.json', executable: false }
];

function download(url, destPath) {
    return new Promise((resolve, reject) => {
        const client = url.startsWith('https') ? https : http;

        client.get(url, (res) => {
            if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
                return download(res.headers.location, destPath).then(resolve).catch(reject);
            }

            if (res.statusCode !== 200) {
                return reject(new Error(`Failed to download ${url}: HTTP status ${res.statusCode}`));
            }

            const fileStream = fs.createWriteStream(destPath);
            res.pipe(fileStream);

            fileStream.on('finish', () => {
                fileStream.close(resolve);
            });

            fileStream.on('error', (err) => {
                fs.unlink(destPath, () => {});
                reject(err);
            });
        }).on('error', (err) => {
            fs.unlink(destPath, () => {});
            reject(err);
        });
    });
}

async function setup() {
    const targetDir = process.cwd();
    console.log(`[+] Setting up Agent v2 Server in: ${targetDir}`);

    // Auto-detect Pterodactyl container port
    const port = process.env.SERVER_PORT || process.env.PORT || '1337';
    console.log(`[+] Configured Unified Port (SSH + Bot): ${port}`);

    // Download or copy configs if missing
    for (const item of FILES_TO_DOWNLOAD) {
        const dest = path.join(targetDir, item.filename);
        if (!fs.existsSync(dest)) {
            try {
                process.stdout.write(`[*] Downloading ${item.filename}... `);
                const nocacheUrl = `${item.url}?t=${Date.now()}`;
                await download(nocacheUrl, dest);
                console.log('OK');
            } catch (err) {
                console.log('FAILED');
                console.error(`[!] Error with ${item.filename}: ${err.message}`);
            }
        } else {
            console.log(`[*] ${item.filename} already exists.`);
        }
    }

    // Locate agentv2 binary
    let agentBinPath = path.join(targetDir, 'agentv2');
    if (!fs.existsSync(agentBinPath)) {
        // Check relative folder ../agentv2/agentv2
        const relativeBin = path.join(__dirname, '..', 'agentv2', 'agentv2');
        if (fs.existsSync(relativeBin)) {
            agentBinPath = relativeBin;
        }
    }

    if (fs.existsSync(agentBinPath)) {
        fs.chmodSync(agentBinPath, 0o755);
        console.log(`[+] Launching Agent v2 Unified Server (${agentBinPath})...`);

        const args = ['-p', String(port)];
        const child = spawn(agentBinPath, args, {
            cwd: targetDir,
            stdio: 'inherit'
        });

        child.on('error', (err) => {
            console.error(`[!] Failed to start agentv2 process: ${err.message}`);
            process.exit(1);
        });

        child.on('exit', (code, signal) => {
            console.log(`[!] agentv2 process exited with code ${code} (signal: ${signal})`);
            process.exit(code || 0);
        });

        process.on('SIGINT', () => child.kill('SIGINT'));
        process.on('SIGTERM', () => child.kill('SIGTERM'));
    } else {
        console.error('[-] agentv2 binary not found. Please compile or provide agentv2.');
        process.exit(1);
    }
}

setup().catch((err) => {
    console.error(`[!] Setup failed: ${err.message}`);
    process.exit(1);
});
