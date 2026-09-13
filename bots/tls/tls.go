package main

import (
	"crypto/tls"
	"fmt"
	"os"
	"strconv"
	"sync"
	"sync/atomic"
	"time"

	"github.com/valyala/fasthttp"
)

type statsType struct {
	total   int64
	success int64
	failed  int64
}

var stats statsType

func buildClient() *fasthttp.Client {
	return &fasthttp.Client{
		TLSConfig: &tls.Config{
			InsecureSkipVerify: true,
			CurvePreferences:   []tls.CurveID{tls.X25519, tls.CurveP256},
			MinVersion:         tls.VersionTLS13,
			MaxVersion:         tls.VersionTLS13,
		},
		MaxConnsPerHost:     10000,
		MaxIdleConnDuration: 30 * time.Second,
		ReadTimeout:         10 * time.Second,
		WriteTimeout:        10 * time.Second,
	}
}

func setHeaders(req *fasthttp.Request) {
	req.Header.Set("User-Agent", "")
	req.Header.Set("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/apng,*/*;q=0.8,application/signed-exchange;v=b3;q=0.7")
	req.Header.Set("Accept-Encoding", "gzip, deflate, br")
	req.Header.Set("Accept-Language", "en-US,en;q=0.9,id;q=0.8")
	req.Header.Set("Cache-Control", "max-age=0")
	req.Header.Set("Sec-Ch-Ua", `"Chromium";v="136", "Google Chrome";v="136", "Not-A.Brand";v="99"`)
	req.Header.Set("Sec-Fetch-Dest", "document")
	req.Header.Set("Sec-Fetch-Mode", "navigate")
	req.Header.Set("Sec-Fetch-Site", "none")
	req.Header.Set("Sec-Fetch-User", "?1")
	req.Header.Set("Upgrade-Insecure-Requests", "1")
	req.Header.Set("Dnt", "1")
}

func makeRequest(client *fasthttp.Client, target string) bool {
	req := fasthttp.AcquireRequest()
	resp := fasthttp.AcquireResponse()
	defer fasthttp.ReleaseRequest(req)
	defer fasthttp.ReleaseResponse(resp)

	req.SetRequestURI(target)
	req.Header.SetMethod(fasthttp.MethodGet)
	setHeaders(req)

	err := client.DoTimeout(req, resp, 10*time.Second)
	if err != nil {
		return false
	}

	return resp.StatusCode() >= 200 && resp.StatusCode() < 500
}

func main() {
	if len(os.Args) < 4 {
		fmt.Println("Usage: hget <url> <duration_seconds> <rate_per_second>")
		fmt.Println("Example: hget https://example.com 10 100")
		os.Exit(1)
	}

	target := os.Args[1]
	duration, err := strconv.Atoi(os.Args[2])
	if err != nil || duration <= 0 {
		fmt.Println("Error: duration must be a positive integer (seconds)")
		os.Exit(1)
	}
	rate, err := strconv.Atoi(os.Args[3])
	if err != nil || rate <= 0 {
		fmt.Println("Error: rate must be a positive integer (req/s)")
		os.Exit(1)
	}

	client := buildClient()

	interval := time.Second / time.Duration(rate)
	ticker := time.NewTicker(interval)
	deadline := time.After(time.Duration(duration) * time.Second)
	var wg sync.WaitGroup

	fmt.Printf("Starting fasthttp requests to %s\n", target)
	fmt.Printf("Duration: %ds | Rate: %d req/s\n\n", duration, rate)

	start := time.Now()

loop:
	for {
		select {
		case <-deadline:
			break loop
		case <-ticker.C:
			wg.Add(1)
			atomic.AddInt64(&stats.total, 1)
			go func() {
				defer wg.Done()

				if makeRequest(client, target) {
					atomic.AddInt64(&stats.success, 1)
				} else {
					atomic.AddInt64(&stats.failed, 1)
				}
			}()
		}
	}

	ticker.Stop()
	wg.Wait()
	elapsed := time.Since(start).Seconds()

	fmt.Println("========== SUMMARY ==========")
	fmt.Printf("Target      : %s\n", target)
	fmt.Printf("Duration    : %.2fs\n", elapsed)
	fmt.Printf("Total Req   : %d\n", stats.total)
	fmt.Printf("Success     : %d\n", stats.success)
	fmt.Printf("Failed      : %d\n", stats.failed)
	fmt.Printf("Avg Rate    : %.2f req/s\n", float64(stats.total)/elapsed)
	fmt.Println("=============================")
}
