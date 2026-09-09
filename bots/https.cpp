// https.cpp
// Simple HTTPS (HTTP/1.1 over TLS) client sender, rate-limited to 30 requests/second
// Usage: ./https <url> <port> <duration_seconds>
//
// Compile: g++ -O2 -o https https.cpp -lssl -lcrypto
// Contoh:
//   ./https example.com 443 10
//   ./https example.com/ping 443 10
//   ./https https://example.com/api/test 443 10

#include <iostream>
#include <string>
#include <cstring>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cctype>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

static inline std::string trim(const std::string& s) {
    auto start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Pisahkan host dan path dari URL yang diberikan user.
static void parse_url(const std::string& url_in, std::string& host, std::string& path, std::string& port_out) {
    std::string url = trim(url_in);

    // Buang skema jika ada (http:// atau https://)
    size_t scheme_pos = url.find("://");
    if (scheme_pos != std::string::npos) {
        url = url.substr(scheme_pos + 3);
    }

    // Cari posisi path pertama ('/')
    size_t slash_pos = url.find('/');
    if (slash_pos == std::string::npos) {
        host = url;
        path = "/";
    } else {
        host = url.substr(0, slash_pos);
        path = url.substr(slash_pos); // termasuk '/'
    }

    // Jika host mengandung port (cth: example.com:8443)
    size_t colon_pos = host.find(':');
    if (colon_pos != std::string::npos) {
        if (port_out.empty()) {
            port_out = host.substr(colon_pos + 1);
        }
        host = host.substr(0, colon_pos);
    }

    host = trim(host);
    path = trim(path);
    if (host.empty()) host = url_in;
    if (path.empty()) path = "/";
}

static int connect_to(const std::string& host, const std::string& port) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int rc = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0) {
        std::cerr << "getaddrinfo error: " << gai_strerror(rc) << "\n";
        return -1;
    }

    int sock = -1;
    for (struct addrinfo* p = res; p != nullptr; p = p->ai_next) {
        sock = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, p->ai_addr, p->ai_addrlen) == 0) break;
        close(sock);
        sock = -1;
    }
    freeaddrinfo(res);
    return sock;
}

static bool send_request(SSL_CTX* ctx, const std::string& host, const std::string& port, const std::string& path) {
    int sock = connect_to(host, port);
    if (sock < 0) {
        std::cerr << "connect failed\n";
        return false;
    }

    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, sock);

    // SNI (Server Name Indication) - wajib untuk kebanyakan server modern
    SSL_set_tlsext_host_name(ssl, host.c_str());

    if (SSL_connect(ssl) <= 0) {
        std::cerr << "TLS handshake gagal\n";
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        close(sock);
        return false;
    }

    std::string req =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Connection: close\r\n"
        "User-Agent: simple-https-client/1.0\r\n"
        "\r\n";

    if (SSL_write(ssl, req.c_str(), (int)req.size()) <= 0) {
        std::cerr << "SSL_write gagal\n";
        ERR_print_errors_fp(stderr);
        SSL_shutdown(ssl);
        SSL_free(ssl);
        close(sock);
        return false;
    }

    // Baca sedikit dari response (baris status) saja
    char buf[512];
    int n = SSL_read(ssl, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::string resp(buf);
        size_t eol = resp.find("\r\n");
        std::string status_line = (eol != std::string::npos) ? resp.substr(0, eol) : resp;
        std::cout << status_line << "\n";
    } else {
        std::cout << "(no response)\n";
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(sock);
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <url> <port> <duration_seconds>\n";
        std::cerr << "Contoh: " << argv[0] << " example.com/ping 443 10\n";
        return 1;
    }

    std::string port = trim(argv[2]);
    std::string host, path;
    parse_url(argv[1], host, path, port);

    if (port.empty()) port = "443";

    int duration_sec = std::atoi(trim(argv[3]).c_str());
    if (duration_sec <= 0) duration_sec = 10;

    std::cout << "Target -> host: " << host << " | path: " << path << " | port: " << port << "\n";

    // Inisialisasi OpenSSL
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    const SSL_METHOD* method = TLS_client_method();
    SSL_CTX* ctx = SSL_CTX_new(method);
    if (!ctx) {
        std::cerr << "Gagal membuat SSL_CTX\n";
        ERR_print_errors_fp(stderr);
        return 1;
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, NULL);

    const int RPS = 30;
    const auto interval = std::chrono::milliseconds(1000 / RPS); // ~33ms per request

    auto start = std::chrono::steady_clock::now();
    auto end_time = start + std::chrono::seconds(duration_sec);

    int count = 0;
    while (std::chrono::steady_clock::now() < end_time) {
        auto tick_start = std::chrono::steady_clock::now();

        count++;
        std::cout << "[" << count << "] ";
        send_request(ctx, host, port, path);

        auto tick_end = std::chrono::steady_clock::now();
        auto elapsed = tick_end - tick_start;
        if (elapsed < interval) {
            std::this_thread::sleep_for(interval - elapsed);
        }
    }

    SSL_CTX_free(ctx);
    std::cout << "Selesai. Total request terkirim: " << count << "\n";
    return 0;
}