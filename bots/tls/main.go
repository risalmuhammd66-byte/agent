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

type Stats struct {
	Total   int64
	Success int64
	Failed  int64
}

var stats Stats

func makeFastHTTPRequest(client *fasthttp.Client, target string) bool {
	req := fasthttp.AcquireRequest()
	resp := fasthttp.AcquireResponse()
	defer fasthttp.ReleaseRequest(req)
	defer fasthttp.ReleaseResponse(resp)

	req.SetRequestURI(target)
	req.Header.SetMethod(fasthttp.MethodGet)

	// Modern browser headers
	req.Header.Set("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/136.0.0.0 Safari/537.36")
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

	err := client.DoTimeout(req, resp, 10*time.Second)
	if err != nil {
		return false
	}

	statusCode := resp.StatusCode()
	return statusCode >= 200 && statusCode < 500
}

func main() {
	if len(os.Args) < 4 {
		fmt.Println("Usage: tls <url> <duration_seconds> <rate_per_second>")
		fmt.Println("Example: ./tls https://example.com 10 100")
		os.Exit(1)
	}

	target := os.Args[1]
	duration, err := strconv.Atoi(os.Args[2])
	if err != nil || duration <= 0 {
		fmt.Fprintln(os.Stderr, "Error: duration harus berupa integer positif.")
		os.Exit(1)
	}

	rate, err := strconv.Atoi(os.Args[3])
	if err != nil || rate <= 0 {
		fmt.Fprintln(os.Stderr, "Error: rate harus berupa integer positif.")
		os.Exit(1)
	}

	// Fasthttp client with custom TLS config
	client := &fasthttp.Client{
		ReadTimeout:                   10 * time.Second,
		WriteTimeout:                  10 * time.Second,
		MaxConnsPerHost:               100000,
		MaxIdleConnDuration:           30 * time.Second,
		NoDefaultUserAgentHeader:      true,
		DisableHeaderNamesNormalizing: true,
		TLSConfig: &tls.Config{
			InsecureSkipVerify: true,
			MinVersion:         tls.VersionTLS12,
			MaxVersion:         tls.VersionTLS13,
		},
	}

	fmt.Printf("Starting fasthttp requests to %s\n", target)
	fmt.Printf("Duration: %ds | Rate: %d req/s\n\n", duration, rate)

	interval := time.Second / time.Duration(rate)
	startTime := time.Now()
	deadline := startTime.Add(time.Duration(duration) * time.Second)

	var wg sync.WaitGroup

	ticker := time.NewTicker(interval)
	defer ticker.Stop()

	for time.Now().Before(deadline) {
		<-ticker.C
		if time.Now().After(deadline) {
			break
		}

		atomic.AddInt64(&stats.Total, 1)
		wg.Add(1)
		go func() {
			defer wg.Done()
			if makeFastHTTPRequest(client, target) {
				atomic.AddInt64(&stats.Success, 1)
			} else {
				atomic.AddInt64(&stats.Failed, 1)
			}
		}()
	}

	wg.Wait()

	elapsed := time.Since(startTime).Seconds()
	total := atomic.LoadInt64(&stats.Total)
	success := atomic.LoadInt64(&stats.Success)
	failed := atomic.LoadInt64(&stats.Failed)

	var avgRate float64
	if elapsed > 0 {
		avgRate = float64(total) / elapsed
	}

	fmt.Println("========== SUMMARY ==========")
	fmt.Printf("Target      : %s\n", target)
	fmt.Printf("Duration    : %.2fs\n", elapsed)
	fmt.Printf("Total Req   : %d\n", total)
	fmt.Printf("Success     : %d\n", success)
	fmt.Printf("Failed      : %d\n", failed)
	fmt.Printf("Avg Rate    : %.2f req/s\n", avgRate)
	fmt.Println("=============================")
}
