// Host-side test of the kernel TLS 1.3 client against real servers.
// g++ -std=gnu++20 -O2 -Ikernel -o /tmp/tls_test tools/tls_test.cpp kernel/net/tls.cpp kernel/net/crypto.cpp
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
#include <random>
#include "net/tls.h"

void* kmalloc(size_t n) { return malloc(n); }
void kfree(void* p) { free(p); }
uint64_t cpu_random64() { static std::random_device rd; return ((uint64_t)rd() << 32) | rd(); }

static int sock_send(void* ctx, const uint8_t* d, size_t n) { return (int)send(*(int*)ctx, d, n, 0); }

int main(int argc, char** argv) {
    const char* host = argc > 1 ? argv[1] : "example.com";
    addrinfo hints{}, *res;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, "443", &hints, &res)) { printf("dns failed\n"); return 1; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(fd, res->ai_addr, res->ai_addrlen)) { printf("connect failed\n"); return 1; }
    tls::Client c(host, sock_send, &fd);
    c.start();
    bool sent = false;
    size_t total = 0;
    uint8_t buf[16384];
    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        c.on_receive(buf, n);
        if (c.failed()) { printf("TLS FAILED: %s\n", c.error()); return 1; }
        if (c.ready() && !sent) {
            char req[512];
            snprintf(req, sizeof(req), "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: KleinOS\r\n\r\n", host);
            c.write((const uint8_t*)req, strlen(req));
            sent = true;
        }
        size_t got;
        while ((got = c.read(buf, sizeof(buf))) > 0) {
            if (total == 0) { fwrite(buf, 1, got < 200 ? got : 200, stdout); printf("\n...\n"); }
            total += got;
        }
        if (c.closed()) break;
    }
    printf("%s: handshake %s, %zu bytes of application data\n", host, c.ready() ? "OK" : "incomplete", total);
    return c.ready() && total > 0 ? 0 : 1;
}
