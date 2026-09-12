package multiplexer

import (
	"bytes"
	"fmt"
	"io"
	"net"
	"sync"
	"time"

	"agentv2/pkg/master"
)

type Multiplexer struct {
	port            int
	internalSSHPort int
	botMgr          *master.BotManager
	listener        net.Listener
	stopChan        chan struct{}
	wg              sync.WaitGroup
}

func NewMultiplexer(port int, internalSSHPort int, botMgr *master.BotManager) *Multiplexer {
	return &Multiplexer{
		port:            port,
		internalSSHPort: internalSSHPort,
		botMgr:          botMgr,
		stopChan:        make(chan struct{}),
	}
}

func (m *Multiplexer) Start() error {
	addr := fmt.Sprintf("0.0.0.0:%d", m.port)
	l, err := net.Listen("tcp", addr)
	if err != nil {
		return fmt.Errorf("failed to listen on %s: %w", addr, err)
	}
	m.listener = l

	fmt.Printf("[+] Single Port Multiplexer active on port %d (SSH & Bot unified)\n", m.port)

	m.wg.Add(1)
	go m.acceptLoop()
	return nil
}

func (m *Multiplexer) Stop() {
	close(m.stopChan)
	if m.listener != nil {
		_ = m.listener.Close()
	}
	m.wg.Wait()
}

func (m *Multiplexer) acceptLoop() {
	defer m.wg.Done()

	for {
		conn, err := m.listener.Accept()
		if err != nil {
			select {
			case <-m.stopChan:
				return
			default:
				return
			}
		}

		go m.route(conn)
	}
}

func (m *Multiplexer) route(conn net.Conn) {
	// Set initial read deadline to sniff protocol
	_ = conn.SetReadDeadline(time.Now().Add(10 * time.Second))

	buf := make([]byte, 256)
	n, err := conn.Read(buf)
	_ = conn.SetReadDeadline(time.Time{}) // clear timeout

	if err != nil || n <= 0 {
		_ = conn.Close()
		return
	}

	initialData := buf[:n]

	// Check SSH banner: "SSH-"
	if n >= 4 && bytes.Equal(initialData[:4], []byte("SSH-")) {
		m.proxyToSSH(conn, initialData)
	} else {
		m.botMgr.HandleBotConnection(conn, initialData)
	}
}

func (m *Multiplexer) proxyToSSH(clientConn net.Conn, initialData []byte) {
	backendConn, err := net.Dial("tcp", fmt.Sprintf("127.0.0.1:%d", m.internalSSHPort))
	if err != nil {
		_ = clientConn.Close()
		return
	}

	// Forward initial SSH banner to internal SSH server
	if _, err := backendConn.Write(initialData); err != nil {
		_ = clientConn.Close()
		_ = backendConn.Close()
		return
	}

	// Bi-directional pipe
	var pipeWg sync.WaitGroup
	pipeWg.Add(2)

	go func() {
		defer pipeWg.Done()
		_, _ = io.Copy(backendConn, clientConn)
		_ = backendConn.Close()
	}()

	go func() {
		defer pipeWg.Done()
		_, _ = io.Copy(clientConn, backendConn)
		_ = clientConn.Close()
	}()

	pipeWg.Wait()
}
