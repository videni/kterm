/* kterm-mdns.c
 *
 * This file is part of kterm
 *
 * This is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Library General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

/*
 * Machines of the local network found by name, without knowing their
 * address: Multicast DNS (RFC 6762) and DNS service discovery (RFC 6763),
 * what macOS calls Bonjour. A Mac with remote login on answers for
 * "<its name>.local" and offers "_ssh._tcp"; an address handed out anew
 * by the router changes nothing.
 *
 *   kterm-mdns resolve NAME.local        its IPv4 address
 *   kterm-mdns browse _ssh._tcp          the machines offering a service:
 *                                        instance, host, port, address,
 *                                        tab separated, one a line
 *   kterm-mdns connect HOST PORT         a connection on standard input and
 *                                        output, for ssh's ProxyCommand
 *
 * Nothing runs in the background, which would wake the device for every
 * announcement on the network: a question is asked when needed. Asked
 * from a port other than 5353, it is answered to that port directly
 * ("legacy unicast", RFC 6762 6.7). The Kindle's firewall lets those
 * answers in once launch.sh allows UDP from port 5353.
 *
 * connect first tries the address found last time (kept in
 * MDNS_CACHE_DIR): no question at all while it still works.
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MDNS_ADDRESS "224.0.0.251"
#define MDNS_PORT 5353
#define TYPE_A 1
#define TYPE_PTR 12
#define TYPE_SRV 33
#define CLASS_IN 1
/** A question is asked again after this long without an answer, ms */
#define RETRY_MS 400
/** Answers are waited for this long, ms */
#define RESOLVE_MS 2000
#define BROWSE_MS 1500
/** A connection to the address found last time is given this long, ms */
#define CACHED_CONNECT_MS 1500
#define CONNECT_MS 4000
#define MDNS_CACHE_DIR "/tmp/kterm-mdns"
#define NAME_MAX_LEN 256
#define MAX_RECORDS 64

/** What the answers told */
typedef struct {
    int type;
    char name[NAME_MAX_LEN];
    /** PTR, SRV: the name pointed to */
    char target[NAME_MAX_LEN];
    uint16_t port;
    struct in_addr address;
} Record;

static Record records[MAX_RECORDS];
static int record_count = 0;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/**
 * @return The name without a final dot, lower case, in place
 */
static char * canonical(char *name) {
    size_t len = strlen(name);
    if (len && name[len - 1] == '.') {
        name[len - 1] = '\0';
    }
    for (char *c = name; *c; c++) {
        if (*c >= 'A' && *c <= 'Z') {
            *c = (char) (*c - 'A' + 'a');
        }
    }
    return name;
}

/**
 * Write a name in DNS form: labels, each after its length
 * @return Bytes written, 0 if it does not fit
 */
static size_t put_name(uint8_t *out, size_t room, const char *name) {
    size_t at = 0;
    const char *label = name;
    while (*label) {
        const char *dot = strchr(label, '.');
        const size_t len = dot ? (size_t) (dot - label) : strlen(label);
        if (len == 0 || len > 63 || at + len + 2 > room) {
            return 0;
        }
        out[at++] = (uint8_t) len;
        memcpy(out + at, label, len);
        at += len;
        label += len + (dot ? 1 : 0);
    }
    out[at++] = 0;
    return at;
}

/**
 * Read a name, following compression pointers
 * @param message Whole message
 * @param size Its size
 * @param at Where the name starts; set past it
 * @param name Set to the name, dotted
 * @return 0, -1 if malformed
 */
static int get_name(const uint8_t *message, size_t size, size_t *at, char *name) {
    size_t pos = *at, out = 0;
    int jumps = 0;
    int jumped = 0;
    name[0] = '\0';
    while (pos < size) {
        const uint8_t len = message[pos];
        if (len == 0) {
            if (!jumped) {
                *at = pos + 1;
            }
            name[out ? out - 1 : 0] = '\0';
            return 0;
        }
        if ((len & 0xc0) == 0xc0) {
            if (pos + 1 >= size || ++jumps > 32) {
                return -1;
            }
            if (!jumped) {
                *at = pos + 2;
            }
            jumped = 1;
            pos = (size_t) ((len & 0x3f) << 8 | message[pos + 1]);
            continue;
        }
        if (pos + 1 + len > size || out + len + 1 >= NAME_MAX_LEN) {
            return -1;
        }
        memcpy(name + out, message + pos + 1, len);
        out += len;
        name[out++] = '.';
        pos += 1 + len;
    }
    return -1;
}

static uint16_t get16(const uint8_t *p) {
    return (uint16_t) (p[0] << 8 | p[1]);
}

/**
 * Keep the records of an answer
 * @return 0, -1 if malformed
 */
static int read_answer(const uint8_t *message, size_t size) {
    if (size < 12) {
        return -1;
    }
    // answers only, not questions
    if (!(message[2] & 0x80)) {
        return 0;
    }
    const int questions = get16(message + 4);
    const int answers = get16(message + 6) + get16(message + 8) + get16(message + 10);
    size_t at = 12;
    char name[NAME_MAX_LEN];
    for (int i = 0; i < questions; i++) {
        if (get_name(message, size, &at, name) || at + 4 > size) {
            return -1;
        }
        at += 4;
    }
    for (int i = 0; i < answers; i++) {
        if (get_name(message, size, &at, name) || at + 10 > size) {
            return -1;
        }
        const int type = get16(message + at);
        const int class = get16(message + at + 2) & 0x7fff;
        const size_t length = get16(message + at + 8);
        at += 10;
        if (at + length > size) {
            return -1;
        }
        size_t data = at;
        at += length;
        if (class != CLASS_IN || record_count >= MAX_RECORDS) {
            continue;
        }
        Record *record = &records[record_count];
        memset(record, 0, sizeof(*record));
        record->type = type;
        snprintf(record->name, sizeof(record->name), "%s", name);
        canonical(record->name);
        if (type == TYPE_A && length == 4) {
            memcpy(&record->address, message + data, 4);
        } else if (type == TYPE_PTR) {
            if (get_name(message, size, &data, record->target)) {
                continue;
            }
            canonical(record->target);
        } else if (type == TYPE_SRV && length > 6) {
            record->port = get16(message + data + 4);
            data += 6;
            if (get_name(message, size, &data, record->target)) {
                continue;
            }
            canonical(record->target);
        } else {
            continue;
        }
        record_count++;
    }
    return 0;
}

/**
 * @return A socket to ask from, -1 on failure
 */
static int open_socket(void) {
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("kterm-mdns: socket");
        return -1;
    }
    const unsigned char ttl = 255;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    return sock;
}

/**
 * Ask for the records of a name
 */
static void ask(int sock, const char *name, int type) {
    uint8_t query[NAME_MAX_LEN + 32];
    memset(query, 0, 12);
    query[5] = 1; // one question
    const size_t len = put_name(query + 12, sizeof(query) - 16, name);
    if (!len) {
        return;
    }
    size_t at = 12 + len;
    query[at++] = 0;
    query[at++] = (uint8_t) type;
    query[at++] = 0;
    query[at++] = CLASS_IN;
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons(MDNS_PORT);
    inet_pton(AF_INET, MDNS_ADDRESS, &to.sin_addr);
    sendto(sock, query, at, 0, (struct sockaddr *) &to, sizeof(to));
}

/**
 * Take the answers coming in until a time
 * @param until Monotonic time, ms
 */
static void listen_until(int sock, long long until) {
    uint8_t message[9000];
    for (;;) {
        const long long left = until - now_ms();
        if (left <= 0) {
            return;
        }
        struct pollfd pfd = { sock, POLLIN, 0 };
        if (poll(&pfd, 1, (int) left) <= 0) {
            return;
        }
        const ssize_t size = recv(sock, message, sizeof(message), 0);
        if (size > 0) {
            read_answer(message, (size_t) size);
        }
    }
}

static const Record * find(int type, const char *name) {
    for (int i = 0; i < record_count; i++) {
        if (records[i].type == type && !strcmp(records[i].name, name)) {
            return &records[i];
        }
    }
    return NULL;
}

/**
 * @param name Name, canonical
 * @param address Set to its address
 * @return 0, -1 if no answer came
 */
static int resolve(const char *name, struct in_addr *address) {
    const int sock = open_socket();
    if (sock < 0) {
        return -1;
    }
    const long long end = now_ms() + RESOLVE_MS;
    const Record *found = NULL;
    while (!found && now_ms() < end) {
        ask(sock, name, TYPE_A);
        const long long next = now_ms() + RETRY_MS;
        while (!found && now_ms() < next) {
            listen_until(sock, next < end ? next : end);
            found = find(TYPE_A, name);
        }
    }
    close(sock);
    if (!found) {
        return -1;
    }
    *address = found->address;
    return 0;
}

/**
 * Print the machines offering a service
 * @param service Service, e.g. "_ssh._tcp"
 */
static int browse(const char *service) {
    char type[NAME_MAX_LEN];
    snprintf(type, sizeof(type), "%s.local", service);
    canonical(type);
    const int sock = open_socket();
    if (sock < 0) {
        return 1;
    }
    ask(sock, type, TYPE_PTR);
    listen_until(sock, now_ms() + RETRY_MS);
    ask(sock, type, TYPE_PTR);
    listen_until(sock, now_ms() + BROWSE_MS - RETRY_MS);
    // what the answers left out is asked for
    int missing = 0;
    for (int i = 0; i < record_count; i++) {
        if (records[i].type != TYPE_PTR || strcmp(records[i].name, type)) {
            continue;
        }
        const Record *srv = find(TYPE_SRV, records[i].target);
        if (!srv) {
            ask(sock, records[i].target, TYPE_SRV);
            missing = 1;
        } else if (!find(TYPE_A, srv->target)) {
            ask(sock, srv->target, TYPE_A);
            missing = 1;
        }
    }
    if (missing) {
        listen_until(sock, now_ms() + RETRY_MS * 2);
        for (int i = 0; i < record_count; i++) {
            if (records[i].type == TYPE_SRV && !find(TYPE_A, records[i].target)) {
                ask(sock, records[i].target, TYPE_A);
                missing = 2;
            }
        }
        if (missing == 2) {
            listen_until(sock, now_ms() + RETRY_MS * 2);
        }
    }
    close(sock);
    const int count = record_count;
    for (int i = 0; i < count; i++) {
        if (records[i].type != TYPE_PTR || strcmp(records[i].name, type)) {
            continue;
        }
        // a machine answering twice is listed once
        int seen = 0;
        for (int j = 0; j < i && !seen; j++) {
            seen = records[j].type == TYPE_PTR && !strcmp(records[j].target, records[i].target);
        }
        const Record *srv = find(TYPE_SRV, records[i].target);
        if (seen || !srv) {
            continue;
        }
        const Record *a = find(TYPE_A, srv->target);
        char instance[NAME_MAX_LEN];
        snprintf(instance, sizeof(instance), "%s", records[i].target);
        const size_t suffix = strlen(type) + 1;
        if (strlen(instance) > suffix) {
            instance[strlen(instance) - suffix] = '\0';
        }
        // DNS escapes in the instance name (spaces, dots) are kept as they are
        printf("%s\t%s\t%u\t%s\n", instance, srv->target, srv->port,
               a ? inet_ntoa(a->address) : "");
    }
    return 0;
}

/**
 * Connect, giving up after a while
 * @return Connected socket, -1 on failure
 */
static int connect_to(struct in_addr address, int port, int timeout_ms) {
    const int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t) port);
    to.sin_addr = address;
    const int flags = fcntl(sock, F_GETFL);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    if (connect(sock, (struct sockaddr *) &to, sizeof(to)) < 0) {
        if (errno != EINPROGRESS) {
            close(sock);
            return -1;
        }
        struct pollfd pfd = { sock, POLLOUT, 0 };
        int error = 0;
        socklen_t len = sizeof(error);
        if (poll(&pfd, 1, timeout_ms) <= 0
            || getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len) < 0 || error) {
            close(sock);
            return -1;
        }
    }
    fcntl(sock, F_SETFL, flags);
    return sock;
}

static void cache_path(const char *host, char *path, size_t size) {
    snprintf(path, size, "%s/%s", MDNS_CACHE_DIR, host);
}

static int cache_read(const char *host, struct in_addr *address) {
    char path[NAME_MAX_LEN + 32], text[32] = "";
    cache_path(host, path, sizeof(path));
    FILE *file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    const int ok = fgets(text, sizeof(text), file) != NULL;
    fclose(file);
    text[strcspn(text, "\n")] = '\0';
    return ok && inet_pton(AF_INET, text, address) == 1 ? 0 : -1;
}

static void cache_write(const char *host, struct in_addr address) {
    char path[NAME_MAX_LEN + 32];
    mkdir(MDNS_CACHE_DIR, 0700);
    cache_path(host, path, sizeof(path));
    FILE *file = fopen(path, "w");
    if (file) {
        fprintf(file, "%s\n", inet_ntoa(address));
        fclose(file);
    }
}

/**
 * Copy both ways between standard input and output and the socket, until
 * the far end closes
 */
static int relay(int sock) {
    char buffer[16384];
    struct pollfd pfd[2] = { { STDIN_FILENO, POLLIN, 0 }, { sock, POLLIN, 0 } };
    for (;;) {
        if (poll(pfd, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            return 1;
        }
        if (pfd[0].revents) {
            const ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (n <= 0) {
                shutdown(sock, SHUT_WR);
                pfd[0].fd = -1;
            } else {
                for (ssize_t done = 0; done < n;) {
                    const ssize_t w = write(sock, buffer + done, (size_t) (n - done));
                    if (w <= 0) {
                        return 1;
                    }
                    done += w;
                }
            }
        }
        if (pfd[1].revents) {
            const ssize_t n = read(sock, buffer, sizeof(buffer));
            if (n <= 0) {
                return 0;
            }
            for (ssize_t done = 0; done < n;) {
                const ssize_t w = write(STDOUT_FILENO, buffer + done, (size_t) (n - done));
                if (w <= 0) {
                    return 1;
                }
                done += w;
            }
        }
    }
}

/**
 * ProxyCommand: connect to a machine by name
 */
static int connect_host(const char *given, int port) {
    char host[NAME_MAX_LEN];
    snprintf(host, sizeof(host), "%s", given);
    canonical(host);
    struct in_addr address;
    int sock = -1;
    // a plain address needs no question
    if (inet_pton(AF_INET, host, &address) == 1) {
        sock = connect_to(address, port, CONNECT_MS);
    } else {
        if (!cache_read(host, &address)) {
            sock = connect_to(address, port, CACHED_CONNECT_MS);
        }
        if (sock < 0) {
            if (resolve(host, &address)) {
                fprintf(stderr, "kterm-mdns: %s 没有回应（不在这个局域网里，或者没开机）\n", host);
                return 255;
            }
            sock = connect_to(address, port, CONNECT_MS);
            if (sock >= 0) {
                cache_write(host, address);
            }
        }
    }
    if (sock < 0) {
        fprintf(stderr, "kterm-mdns: 连不上 %s (%s) 的 %d 端口\n", host, inet_ntoa(address), port);
        return 255;
    }
    return relay(sock);
}

static int usage(void) {
    fprintf(stderr, "usage: kterm-mdns resolve NAME.local | browse _SERVICE._tcp | connect HOST PORT\n");
    return 2;
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "resolve")) {
        char name[NAME_MAX_LEN];
        snprintf(name, sizeof(name), "%s", argv[2]);
        struct in_addr address;
        if (resolve(canonical(name), &address)) {
            fprintf(stderr, "kterm-mdns: %s 没有回应\n", name);
            return 1;
        }
        printf("%s\n", inet_ntoa(address));
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "browse")) {
        return browse(argv[2]);
    }
    if (argc == 4 && !strcmp(argv[1], "connect")) {
        return connect_host(argv[2], atoi(argv[3]));
    }
    return usage();
}
