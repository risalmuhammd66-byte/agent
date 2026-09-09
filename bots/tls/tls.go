package main

import (
  "crypto/tls"
  "fmt"
  "io"
  "io/ioutil"
  "net/http"
  "os"
  "runtime"
  "strconv"
  "sync"
  "sync/atomic"
  "time"

  "golang.org/x/net/http2"
)

type Stats struct {
  sent    int64
  success int64
  failed  int64
  bytes   int64
}

func main() {
  debug := false
  args  := []string{}
  for _, a := range os.Args[1:] {
    if a == "-debug" || a == "--debug" {
      debug = true
    } else {
      args = append(args, a)
    }
  }

  if len(args) < 3 {
    fmt.Println("Usage: ./tls <url> <time> <rate> [-debug]")
    fmt.Println("Example: ./tls https://example.com 10 100000 -debug")
    os.Exit(1)
  }

  target   := args[0]
  duration, _ := strconv.Atoi(args[1])
  rate,    _ := strconv.Atoi(args[2])

  if duration <= 0 || rate <= 0 {
    fmt.Println("[-] Invalid time or rate")
    os.Exit(1)
  }

  numCPU := runtime.NumCPU()
  runtime.GOMAXPROCS(numCPU)

  // Workers: goroutine pool
  workers := rate / 5
  if workers < 256 {
    workers = 256
  }
  if workers > 65535 {
    workers = 65535
  }

  useH2 := isHTTPS(target)
  proto  := "HTTP/1.1"
  if useH2 {
    proto = "HTTP/2"
  }

  fmt.Printf("\n[*] Target   : %s\n", target)
  fmt.Printf("[*] Protocol : %s\n", proto)
  fmt.Printf("[*] Duration : %ds\n", duration)
  fmt.Printf("[*] Rate     : %d req/s\n", rate)
  fmt.Printf("[*] Workers  : %d\n", workers)
  fmt.Printf("[*] CPUs     : %d\n", numCPU)
  if debug {
    fmt.Printf("[*] Debug    : ON\n")
  }
  fmt.Println()

  tlsCfg := &tls.Config{
    InsecureSkipVerify: true,
    MinVersion:         tls.VersionTLS12,
    NextProtos:         []string{"h2", "http/1.1"},
    ClientSessionCache: tls.NewLRUClientSessionCache(8192),
  }

  // Build transport — HTTP/2 if HTTPS, plain HTTP/1.1 if HTTP
  var transport http.RoundTripper
  if useH2 {
    t2 := &http2.Transport{
      TLSClientConfig: tlsCfg,
      // H2 multiplexing: many streams per conn
      AllowHTTP: false,
    }
    transport = t2
  } else {
    t1 := &http.Transport{
      MaxIdleConns:        workers,
      MaxIdleConnsPerHost: workers,
      MaxConnsPerHost:     workers,
      IdleConnTimeout:     90 * time.Second,
      DisableCompression:  true,
      ForceAttemptHTTP2:   false,
    }
    transport = t1
  }

  client := &http.Client{
    Transport: transport,
    Timeout:   5 * time.Second,
    CheckRedirect: func(r *http.Request, via []*http.Request) error {
      return http.ErrUseLastResponse
    },
  }

  stats := &Stats{}
  stop  := make(chan struct{})
  end   := time.Now().Add(time.Duration(duration) * time.Second)

  // Atomic token bucket — refill every 1ms
  var tokens    int64
  maxTokens := int64(rate)
  atomic.StoreInt64(&tokens, maxTokens)

  go func() {
    refillPer := int64(rate) / 1000
    if refillPer < 1 {
      refillPer = 1
    }
    t := time.NewTicker(time.Millisecond)
    defer t.Stop()
    for {
      select {
      case <-t.C:
        cur := atomic.LoadInt64(&tokens)
        add := refillPer
        if cur+add > maxTokens {
          add = maxTokens - cur
        }
        if add > 0 {
          atomic.AddInt64(&tokens, add)
        }
      case <-stop:
        return
      }
    }
  }()

  var wg sync.WaitGroup

  for i := 0; i < workers; i++ {
    wg.Add(1)
    go func() {
      defer wg.Done()

      req, _ := http.NewRequest("GET", target, nil)
      req.Header.Set("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36")
      req.Header.Set("Accept", "*/*")
      req.Header.Set("Connection", "keep-alive")

      for {
        if time.Now().After(end) {
          return
        }

        // CAS spin for token
        for {
          cur := atomic.LoadInt64(&tokens)
          if cur <= 0 {
            runtime.Gosched()
            if time.Now().After(end) {
              return
            }
            continue
          }
          if atomic.CompareAndSwapInt64(&tokens, cur, cur-1) {
            break
          }
        }

        if time.Now().After(end) {
          return
        }

        atomic.AddInt64(&stats.sent, 1)

        resp, err := client.Do(req)
        if err != nil {
          atomic.AddInt64(&stats.failed, 1)
          continue
        }

        n, _ := io.Copy(ioutil.Discard, resp.Body)
        resp.Body.Close()
        atomic.AddInt64(&stats.success, 1)
        atomic.AddInt64(&stats.bytes, n)
      }
    }()
  }

  // Stats printer
  if debug {
    go func() {
      const window = 100 * time.Millisecond
      t := time.NewTicker(window)
      defer t.Stop()
      var prevSent, prevOK, prevFail int64
      elapsed := 0.0
      for {
        select {
        case <-t.C:
          elapsed += 0.1
          s := atomic.LoadInt64(&stats.sent)
          o := atomic.LoadInt64(&stats.success)
          f := atomic.LoadInt64(&stats.failed)
          rps     := float64(s-prevSent) * 10
          rpsOK   := float64(o-prevOK)   * 10
          rpsFail := float64(f-prevFail)  * 10
          prevSent, prevOK, prevFail = s, o, f
          fmt.Printf("\r[%05.1fs] Sent: %-10d | OK: %-10d | Fail: %-8d | RPS: %-9.0f | OK/s: %-9.0f | Fail/s: %-7.0f",
            elapsed, s, o, f, rps, rpsOK, rpsFail)
        case <-stop:
          return
        }
      }
    }()
  } else {
    go func() {
      prev := int64(0)
      t    := time.NewTicker(time.Second)
      defer t.Stop()
      elapsed := 0
      for {
        select {
        case <-t.C:
          elapsed++
          cur := atomic.LoadInt64(&stats.sent)
          fmt.Printf("\r[%02ds] Sent: %-10d | Success: %-10d | Failed: %-8d | RPS: %-8d",
            elapsed,
            cur,
            atomic.LoadInt64(&stats.success),
            atomic.LoadInt64(&stats.failed),
            cur-prev,
          )
          prev = cur
        case <-stop:
          return
        }
      }
    }()
  }

  wg.Wait()
  close(stop)

  total := atomic.LoadInt64(&stats.sent)
  fmt.Printf("\n\n[+] Done!\n")
  fmt.Printf("    Protocol     : %s\n", proto)
  fmt.Printf("    Total Sent   : %d\n", total)
  fmt.Printf("    Success      : %d\n", atomic.LoadInt64(&stats.success))
  fmt.Printf("    Failed       : %d\n", atomic.LoadInt64(&stats.failed))
  fmt.Printf("    Data Received: %.2f MB\n", float64(atomic.LoadInt64(&stats.bytes))/1024/1024)
  fmt.Printf("    Avg RPS      : %.0f\n\n", float64(total)/float64(duration))
}

func isHTTPS(url string) bool {
  return len(url) >= 8 && url[:8] == "https://"
}