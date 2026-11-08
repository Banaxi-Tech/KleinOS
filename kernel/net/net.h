#pragma once
// Network stack: e1000 driver, Ethernet/ARP/IPv4/ICMP/UDP/TCP, DHCP, DNS.
#include <stdint.h>
#include <stddef.h>

namespace net {

// A network card provided by a .dri driver. Register from probe(); it replaces the built-in e1000.
struct NicDriver {
    const char* name;
    uint8_t mac[6];
    void (*send)(const uint8_t* frame, size_t len);              // whole Ethernet frame, no FCS; pad to 60 bytes
    bool (*link_up)();
    void (*poll)(void (*deliver)(const uint8_t* frame, size_t len));   // call deliver for each received frame
};
void register_nic(const NicDriver* nic);

void init();                    // probe NIC, start DHCP
void poll();                    // called every main-loop iteration
bool is_up();                   // link up and IPv4 configured
const char* ip_string();        // "10.0.2.15" or ""
const char* nic_name();         // "Intel 82540EM (e1000)" or "none"

struct Status {
    bool link;
    bool configured;
    uint32_t ip, netmask, gateway, dns;   // host byte order
    uint8_t mac[6];
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes;
};
Status status();
void ip_to_string(uint32_t ip, char* out);   // out >= 16 bytes

// ---- DNS (async). Returns a handle; poll dns_result until != 0.
int dns_resolve(const char* host);
// 0 = pending, 1 = done (ip set), -1 = failed
int dns_result(int handle, uint32_t* ip);

// ---- ICMP echo
// Sends a ping; returns sequence id. ping_result: -1 pending/timeout, else RTT ms.
int ping_send(uint32_t ip);
int ping_result(int seq);

// ---- TCP (non-blocking sockets)
int tcp_connect(uint32_t ip, uint16_t port);  // returns socket id or -1
enum class TcpState { Connecting, Open, Closed, Error };
TcpState tcp_state(int sock);
int tcp_send(int sock, const void* data, size_t len);   // bytes queued
size_t tcp_recv(int sock, void* buf, size_t max);       // bytes read (0 = nothing yet)
size_t tcp_available(int sock);
void tcp_close(int sock);
bool tcp_peer_closed(int sock);                         // FIN received and all data read

}  // namespace net
