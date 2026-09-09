#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>
#include <curl/curl.h>

struct Stats {
    std::atomic<long long> total{0};
    std::atomic<long long> success{0};
    std::atomic<long long> failed{0};
};

Stats stats;

// Fungsi untuk mengirim HTTP request menggunakan libcurl
bool makeRequest(const std::string& target) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    // Set Header
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "User-Agent: ");
    headers = curl_slist_append(headers, "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7");
    headers = curl_slist_append(headers, "Accept-Encoding: gzip, deflate, br");
    headers = curl_slist_append(headers, "Accept-Language: en-US,en;q=0.9,id;q=0.8");
    headers = curl_slist_append(headers, "Cache-Control: max-age=0");
    headers = curl_slist_append(headers, "Sec-Ch-Ua: \"Chromium\";v=\"136\", \"Google Chrome\";v=\"136\", \"Not-A.Brand\";v=\"99\"");
    headers = curl_slist_append(headers, "Sec-Fetch-Dest: document");
    headers = curl_slist_append(headers, "Sec-Fetch-Mode: navigate");
    headers = curl_slist_append(headers, "Sec-Fetch-Site: none");
    headers = curl_slist_append(headers, "Sec-Fetch-User: ?1");
    headers = curl_slist_append(headers, "Upgrade-Insecure-Requests: 1");
    headers = curl_slist_append(headers, "Dnt: 1");

    // Configure cURL options
    curl_easy_setopt(curl, CURLOPT_URL, target.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);               // Read/Write Timeout
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);          // InsecureSkipVerify: true
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_3); // Force TLS 1.3
    
    // Matikan output response body ke stdout
    curl_easy_setopt(curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, +[](void*, size_t size, size_t nmemb, void*) -> size_t {
        return size * nmemb;
    });

    CURLcode res = curl_easy_perform(curl);
    long response_code = 0;

    bool ok = false;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        if (response_code >= 200 && response_code < 500) {
            ok = true;
        }
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return ok;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::cout << "Usage: hget <url> <duration_seconds> <rate_per_second>\n";
        std::cout << "Example: hget https://example.com 10 100\n";
        return 1;
    }

    std::string target = argv[1];
    int duration = std::stoi(argv[2]);
    int rate = std::stoi(argv[3]);

    if (duration <= 0 || rate <= 0) {
        std::cerr << "Error: duration & rate harus berupa integer positif.\n";
        return 1;
    }

    curl_global_init(CURL_GLOBAL_ALL);

    std::cout << "Starting cURL requests to " << target << "\n";
    std::cout << "Duration: " << duration << "s | Rate: " << rate << " req/s\n\n";

    auto interval = std::chrono::microseconds(1000000 / rate);
    auto startTime = std::chrono::steady_clock::now();
    auto deadline = startTime + std::chrono::seconds(duration);

    std::vector<std::thread> threads;

    while (std::chrono::steady_clock::now() < deadline) {
        auto nextTick = std::chrono::steady_clock::now() + interval;

        stats.total++;
        
        // Spawn thread asinkron untuk setiap request (mirip `go func()`)
        threads.emplace_back([target]() {
            if (makeRequest(target)) {
                stats.success++;
            } else {
                stats.failed++;
            }
        });

        // Delay presisi untuk menjaga rate
        std::this_thread::sleep_until(nextTick);
    }

    // Tunggu semua request selesai (mirip `wg.Wait()`)
    for (auto& t : threads) {
        if (t.joinable()) {
            t.join();
        }
    }

    auto endTime = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(endTime - startTime).count();

    curl_global_cleanup();

    std::cout << "========== SUMMARY ==========\n";
    std::cout << "Target      : " << target << "\n";
    std::cout << "Duration    : " << elapsed << "s\n";
    std::cout << "Total Req   : " << stats.total << "\n";
    std::cout << "Success     : " << stats.success << "\n";
    std::cout << "Failed      : " << stats.failed << "\n";
    std::cout << "Avg Rate    : " << stats.total / elapsed << " req/s\n";
    std::cout << "=============================\n";

    return 0;
}