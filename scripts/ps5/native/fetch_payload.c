/*
 * Fetch the title executable from the development host and install it.
 *
 * Large FTP uploads into /system_ex/app are dropped near the end by the
 * console's file server, so the file is pulled from the host instead: this
 * payload is small enough to transfer reliably and, running outside the title
 * sandbox, may write the application image directly.
 *
 * Build:  prospero-clang -O2 fetch_payload.c -o fetch-payload.elf
 * Run:    http://<console>:8084/loadpayload:fetch-payload.elf
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <sys/stat.h>

#ifndef HOST_ADDRESS
#define HOST_ADDRESS "192.168.100.7"
#endif
#ifndef HOST_PORT
#define HOST_PORT 8081
#endif
#ifndef SOURCE_PATH
#define SOURCE_PATH "/eboot.bin"
#endif
#ifndef TARGET_PATH
#define TARGET_PATH "/system_ex/app/PPSA99010/eboot.bin"
#endif
#ifndef TARGET_PARTIAL
#define TARGET_PARTIAL "/system_ex/app/PPSA99010/eboot.bin.part"
#endif

static unsigned int parse_ipv4(const char *text)
{
    unsigned int octets[4] = {0, 0, 0, 0};
    const char *cursor = text;
    for (int index = 0; index < 4; ++index)
    {
        if (*cursor < '0' || *cursor > '9')
            return 0;
        unsigned int value = 0;
        while (*cursor >= '0' && *cursor <= '9')
        {
            value = value * 10 + (unsigned int)(*cursor - '0');
            ++cursor;
        }
        octets[index] = value;
        if (index < 3)
        {
            if (*cursor != '.')
                return 0;
            ++cursor;
        }
    }
    return (octets[3] << 24) | (octets[2] << 16) | (octets[1] << 8) | octets[0];
}

static int write_all(int descriptor, const char *data, size_t size)
{
    while (size > 0)
    {
        ssize_t written = write(descriptor, data, size);
        if (written <= 0)
            return -1;
        data += written;
        size -= (size_t)written;
    }
    return 0;
}

#define LOG_PATH "/user/download/fetch-payload.log"

static void log_two(const char *format, long first, long second)
{
    int log = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log < 0)
        return;
    char line[192];
    int length = snprintf(line, sizeof(line), format, first, second);
    if (length > 0)
        write(log, line, (size_t)length);
    write(log, "\n", 1);
    close(log);
}

static void log_space(const char *path)
{
    struct statvfs info;
    if (statvfs(path, &info) != 0)
    {
        log_two("fetch: statvfs failed errno=%ld path_hash=%ld", (long)errno,
                (long)path[1]);
        return;
    }
    int log = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log < 0)
        return;
    char line[256];
    int length = snprintf(line, sizeof(line),
                          "fetch: %s block=%lu free_blocks=%lu avail=%lu bytes",
                          path, (unsigned long)info.f_frsize,
                          (unsigned long)info.f_bfree, (unsigned long)info.f_bavail);
    if (length > 0)
        write(log, line, (size_t)length);
    write(log, "\n", 1);
    close(log);
}

static unsigned long long tree_bytes(const char *path, int depth)
{
    DIR *directory = opendir(path);
    if (directory == NULL)
        return 0;
    unsigned long long total = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
    {
        if (entry->d_name[0] == '.')
            continue;
        char child[512];
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        struct stat info;
        if (stat(child, &info) != 0)
            continue;
        if (S_ISDIR(info.st_mode))
        {
            if (depth > 0)
                total += tree_bytes(child, depth - 1);
        }
        else
        {
            total += (unsigned long long)info.st_size;
        }
    }
    closedir(directory);
    return total;
}

int main(void)
{
    log_two("fetch: start errno=%ld marker=%ld", 0, 0);
    {
        struct statvfs root;
        if (statvfs("/system_ex", &root) == 0)
        {
            int log = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (log >= 0)
            {
                char line[256];
                int length = snprintf(line, sizeof(line),
                                      "fetch: /system_ex total=%llu MB free=%llu MB",
                                      (unsigned long long)root.f_blocks * root.f_frsize / 1048576ull,
                                      (unsigned long long)root.f_bavail * root.f_frsize / 1048576ull);
                if (length > 0)
                    write(log, line, (size_t)length);
                write(log, "\n", 1);
                close(log);
            }
        }
        static const char *const dirs[] = {"/system_ex/app", "/system_ex/common_ex",
                                           "/system_ex/etc", "/system_ex/mbus",
                                           "/system_ex/priv_ex", "/system_ex/rnps",
                                           "/system_ex/vsh_asset"};
        for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i)
        {
            unsigned long long bytes = tree_bytes(dirs[i], 1);
            int dir_log = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (dir_log >= 0)
            {
                char line[256];
                int length = snprintf(line, sizeof(line), "fetch: %s = %llu MB", dirs[i],
                                      bytes / 1048576ull);
                if (length > 0)
                    write(dir_log, line, (size_t)length);
                write(dir_log, "\n", 1);
                close(dir_log);
            }
        }
        {
            DIR *apps = opendir("/system_ex/app");
            if (apps != NULL)
            {
                struct dirent *entry;
                while ((entry = readdir(apps)) != NULL)
                {
                    if (entry->d_name[0] == '.')
                        continue;
                    char child[512];
                    snprintf(child, sizeof(child), "/system_ex/app/%s", entry->d_name);
                    unsigned long long bytes = tree_bytes(child, 4);
                    if (bytes < 1048576ull)
                        continue;
                    int dir_log = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
                    if (dir_log >= 0)
                    {
                        char line[256];
                        int length = snprintf(line, sizeof(line), "fetch: %s = %llu MB",
                                              child, bytes / 1048576ull);
                        if (length > 0)
                            write(dir_log, line, (size_t)length);
                        write(dir_log, "\n", 1);
                        close(dir_log);
                    }
                }
                closedir(apps);
            }
        }
        unsigned long long app_bytes = tree_bytes("/system_ex/app", 2);
        int log = open(LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log >= 0)
        {
            char line[256];
            int length = snprintf(line, sizeof(line), "fetch: /system_ex/app used=%llu MB",
                                  app_bytes / 1048576ull);
            if (length > 0)
                write(log, line, (size_t)length);
            write(log, "\n", 1);
            close(log);
        }
    }
    log_space("/system_ex/app");
    log_space("/user/download");
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        printf("fetch: socket failed\n");
        return 1;
    }

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_len = sizeof(address);
    address.sin_family = AF_INET;
    address.sin_port = (uint16_t)((HOST_PORT << 8) | (HOST_PORT >> 8));
    address.sin_addr.s_addr = parse_ipv4(HOST_ADDRESS);

    if (connect(sock, (struct sockaddr *)&address, sizeof(address)) != 0)
    {
        printf("fetch: connect to %s:%d failed\n", HOST_ADDRESS, HOST_PORT);
        return 1;
    }

    char request[512];
    int request_length = snprintf(request, sizeof(request),
                                  "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
                                  SOURCE_PATH, HOST_ADDRESS);
    if (write_all(sock, request, (size_t)request_length) != 0)
    {
        printf("fetch: request failed\n");
        return 1;
    }

    static char buffer[65536];
    ssize_t received = recv(sock, buffer, sizeof(buffer), 0);
    if (received <= 0)
    {
        printf("fetch: no response\n");
        return 1;
    }

    char *body = strstr(buffer, "\r\n\r\n");
    if (body == NULL)
    {
        printf("fetch: malformed response\n");
        return 1;
    }
    body += 4;
    size_t body_size = (size_t)received - (size_t)(body - buffer);

    int target = open(TARGET_PARTIAL, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (target < 0)
    {
        printf("fetch: cannot open %s\n", TARGET_PARTIAL);
        return 1;
    }

    size_t total = 0;
    if (write_all(target, body, body_size) != 0)
    {
        printf("fetch: write failed\n");
        close(target);
        return 1;
    }
    total += body_size;

    while ((received = recv(sock, buffer, sizeof(buffer), 0)) > 0)
    {
        if (write_all(target, buffer, (size_t)received) != 0)
        {
            log_two("fetch: write failed after %ld bytes, errno=%ld", (long)total,
                    (long)errno);
            printf("fetch: write failed at %zu bytes\n", total);
            close(target);
            return 1;
        }
        total += (size_t)received;
    }

    log_two("fetch: stream ended total=%ld errno=%ld", (long)total, (long)errno);
    close(target);
    close(sock);

    if (rename(TARGET_PARTIAL, TARGET_PATH) != 0)
    {
        printf("fetch: rename failed\n");
        return 1;
    }

    printf("fetch: installed %zu bytes to %s\n", total, TARGET_PATH);
    return 0;
}
