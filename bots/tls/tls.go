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

const (
	maxConnsPerHost     = 100000
	maxIdleConnDuration = 30 * time.Second
	readTimeout         = 10 * time.Second
	writeTimeout        = 10 * time.Second
)

func buildClient() *fasthttp.Client {
	tlsConfig := &tls.Config{
		InsecureSkipVerify: true,
		MinVersion:         tls.VersionTLS13,
		MaxVersion:         tls.VersionTLS13,
		CurvePreferences: []tls.CurveID{
			tls.X25519,
			tls.CurveP256,
		},
	}

	return &fasthttp.Client{
		TLSConfig: tlsConfig,

		// Connection pooling
		MaxConnsPerHost:     maxConnsPerHost,
		MaxIdleConnDuration: maxIdleConnDuration,

		// Timeouts
		ReadTimeout:  readTimeout,
		WriteTimeout: writeTimeout,
	}
}

func setHeaders(req *fasthttp.Request) {
    req.Header.SetMethod("GET")
    req.Header.Set("User-Agent", "curl/8.7.1")
    req.Header.Set("Accept", "*/*")
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
		fmt.Println("Missing arguments! Usage: tls <url> <duration_seconds> <rate_per_second>")
		fmt.Println("Example: tls https://meji.cc.cd 10 100")
		os.Exit(1)
	}

	target := os.Args[1]

	duration, err := strconv.Atoi(os.Args[2])
	if err != nil || duration <= 0 {
		fmt.Println("Duration's gotta be a positive number (in seconds)")
		os.Exit(1)
	}

	rate, err := strconv.Atoi(os.Args[3])
	if err != nil || rate <= 0 {
		fmt.Println("Rate's gotta be a positive number (req/s)")
		os.Exit(1)
	}

	client := buildClient()

	interval := time.Second / time.Duration(rate)
	ticker := time.NewTicker(interval)
	deadline := time.After(time.Duration(duration) * time.Second)
	var wg sync.WaitGroup

	fmt.Printf("Sent requests to %s\n", target)
	fmt.Printf("Time: %ds | Rate: %d req/s\n\n", duration, rate)

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

	fmt.Println("Here's how it went")
	fmt.Printf("Hit target: %s\n", target)
	fmt.Printf("Time taken: %.2fs\n", elapsed)
	fmt.Printf("Requests fired: %d\n", stats.total)
	fmt.Printf("Landed clean: %d\n", stats.success)
	fmt.Printf("Bounced back: %d\n", stats.failed)
	fmt.Printf("Cruising at: %.2f req/s\n", float64(stats.total)/elapsed)
}
