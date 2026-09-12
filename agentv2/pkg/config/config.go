package config

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"
)

type UserConfig struct {
	Username        string `json:"username"`
	Password        string `json:"password"`
	ConcurrentLimit int    `json:"concurrentLimit"`
	TimeLimit       int    `json:"timeLimit"`
}

type MethodConfig struct {
	Name string `json:"name"`
	Cmd  string `json:"cmd"`
}

type ConfigManager struct {
	mu            sync.RWMutex
	users         []UserConfig
	methods       []MethodConfig
	usersPath     string
	methodsPath   string
	notifyChannel chan string
}

func NewConfigManager(usersFile, methodsFile string) *ConfigManager {
	return &ConfigManager{
		usersPath:     usersFile,
		methodsPath:   methodsFile,
		notifyChannel: make(chan string, 16),
	}
}

func (cm *ConfigManager) NotifyChan() <-chan string {
	return cm.notifyChannel
}

func (cm *ConfigManager) LoadAll() error {
	if err := cm.LoadUsers(); err != nil {
		return err
	}
	return cm.LoadMethods()
}

func (cm *ConfigManager) LoadUsers() error {
	cm.mu.Lock()
	defer cm.mu.Unlock()

	data, err := os.ReadFile(cm.usersPath)
	if err != nil {
		if os.IsNotExist(err) {
			// default user
			defaultUsers := []UserConfig{
				{Username: "root", Password: "password123", ConcurrentLimit: 1, TimeLimit: 300},
			}
			out, _ := json.MarshalIndent(defaultUsers, "", "  ")
			_ = os.WriteFile(cm.usersPath, out, 0644)
			cm.users = defaultUsers
			fmt.Println("[!] users.json not found, created default user root:password123")
			return nil
		}
		return fmt.Errorf("failed to read %s: %w", cm.usersPath, err)
	}

	var loaded []UserConfig
	if err := json.Unmarshal(data, &loaded); err != nil {
		return fmt.Errorf("failed to parse %s: %w", cm.usersPath, err)
	}

	cm.users = loaded
	fmt.Printf("[+] Loaded %d user(s) from %s\n", len(cm.users), cm.usersPath)
	return nil
}

func (cm *ConfigManager) LoadMethods() error {
	cm.mu.Lock()
	defer cm.mu.Unlock()

	data, err := os.ReadFile(cm.methodsPath)
	if err != nil {
		if os.IsNotExist(err) {
			defaultMethods := []MethodConfig{
				{Name: "http", Cmd: "./flood http {host} {port} {time}"},
			}
			out, _ := json.MarshalIndent(defaultMethods, "", "  ")
			_ = os.WriteFile(cm.methodsPath, out, 0644)
			cm.methods = defaultMethods
			fmt.Println("[!] methods.json not found, created default method")
			return nil
		}
		return fmt.Errorf("failed to read %s: %w", cm.methodsPath, err)
	}

	var loaded []MethodConfig
	if err := json.Unmarshal(data, &loaded); err != nil {
		return fmt.Errorf("failed to parse %s: %w", cm.methodsPath, err)
	}

	cm.methods = loaded
	fmt.Printf("[+] Loaded %d method(s) from %s\n", len(cm.methods), cm.methodsPath)
	return nil
}

func (cm *ConfigManager) GetUsers() []UserConfig {
	cm.mu.RLock()
	defer cm.mu.RUnlock()
	res := make([]UserConfig, len(cm.users))
	copy(res, cm.users)
	return res
}

func (cm *ConfigManager) FindUser(username string) (UserConfig, bool) {
	cm.mu.RLock()
	defer cm.mu.RUnlock()
	for _, u := range cm.users {
		if u.Username == username {
			return u, true
		}
	}
	return UserConfig{}, false
}

func (cm *ConfigManager) GetMethods() []MethodConfig {
	cm.mu.RLock()
	defer cm.mu.RUnlock()
	res := make([]MethodConfig, len(cm.methods))
	copy(res, cm.methods)
	return res
}

func (cm *ConfigManager) FindMethod(name string) (MethodConfig, bool) {
	cm.mu.RLock()
	defer cm.mu.RUnlock()
	for _, m := range cm.methods {
		if m.Name == name {
			return m, true
		}
	}
	return MethodConfig{}, false
}

func (cm *ConfigManager) StartWatcher() {
	go func() {
		var lastUsersMod, lastMethodsMod time.Time

		if fi, err := os.Stat(cm.usersPath); err == nil {
			lastUsersMod = fi.ModTime()
		}
		if fi, err := os.Stat(cm.methodsPath); err == nil {
			lastMethodsMod = fi.ModTime()
		}

		ticker := time.NewTicker(1 * time.Second)
		defer ticker.Stop()

		for range ticker.C {
			if fi, err := os.Stat(cm.usersPath); err == nil {
				if fi.ModTime().After(lastUsersMod) {
					lastUsersMod = fi.ModTime()
					time.Sleep(300 * time.Millisecond) // debounce
					if err := cm.LoadUsers(); err == nil {
						select {
						case cm.notifyChannel <- "[~] users.json reloaded":
						default:
						}
					}
				}
			}

			if fi, err := os.Stat(cm.methodsPath); err == nil {
				if fi.ModTime().After(lastMethodsMod) {
					lastMethodsMod = fi.ModTime()
					time.Sleep(300 * time.Millisecond) // debounce
					if err := cm.LoadMethods(); err == nil {
						select {
						case cm.notifyChannel <- "[~] methods.json reloaded":
						default:
						}
					}
				}
			}
		}
	}()
	fmt.Printf("[~] Config watcher active for %s and %s\n", filepath.Base(cm.usersPath), filepath.Base(cm.methodsPath))
}
