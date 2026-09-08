// http.cpp
// Simple HTTP/1.1 client sender, rate-limited to 30 requests/second
// Usage: ./http <url> <port> <duration_seconds>
//
// Compile: g++ -O2 -o http http.cpp
// Contoh:
//   ./http example.com 80 10
//   ./http example.com/ping 80 10
//   ./http http://example.com/api/test 80 10

#include <iostream>
#include <string>
#include <cstring>
#include <chrono>
#include <thread>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>

// Pisahkan host dan path dari URL yang diberikan user.
// Menerima input dengan atau tanpa skema (http://, https://).
static void parse_url(const std::string& url_in, std::string& host, std::string& path) {
    std::string url = url_in;

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

static bool send_request(const std::string& host, const std::string& port, const std::string& path) {
    int sock = connect_to(host, port);
    if (sock < 0) {
        std::cerr << "connect failed\n";
        return false;
    }

    std::string req =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Connection: close\r\n"
        "User-Agent: simple-http1-client/1.0\r\n"
        "\r\n";

    if (send(sock, req.c_str(), req.size(), 0) < 0) {
        std::cerr << "send failed\n";
        close(sock);
        return false;
    }

    // Baca sedikit dari response (baris status) saja
    char buf[512];
    ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = '\0';
        std::string resp(buf);
        size_t eol = resp.find("\r\n");
        std::string status_line = (eol != std::string::npos) ? resp.substr(0, eol) : resp;
        std::cout << status_line << "\n";
    } else {
        std::cout << "(no response)\n";
    }

    close(sock);
    return true;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cerr << "Usage: " << argv[0] << " <url> <port> <duration_seconds>\n";
        std::cerr << "Contoh: " << argv[0] << " example.com/ping 80 10\n";
        return 1;
    }

    std::string host, path;
    parse_url(argv[1], host, path);

    std::string port = argv[2];
    int duration_sec = std::stoi(argv[3]);

    std::cout << "Target -> host: " << host << " | path: " << path << " | port: " << port << "\n";

    const int RPS = 30;
    const auto interval = std::chrono::milliseconds(1000 / RPS); // ~33ms per request

    auto start = std::chrono::steady_clock::now();
    auto end_time = start + std::chrono::seconds(duration_sec);

    int count = 0;
    while (std::chrono::steady_clock::now() < end_time) {
        auto tick_start = std::chrono::steady_clock::now();

        count++;
        std::cout << "[" << count << "] ";
        send_request(host, port, path);

        auto tick_end = std::chrono::steady_clock::now();
        auto elapsed = tick_end - tick_start;
        if (elapsed < interval) {
            std::this_thread::sleep_for(interval - elapsed);
        }
    }

    std::cout << "Selesai. Total request terkirim: " << count << "\n";
    return 0;
}