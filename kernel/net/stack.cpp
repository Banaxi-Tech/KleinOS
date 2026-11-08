// KleinOS network stack: Intel e1000 (82540EM) driver + Ethernet, ARP, IPv4,
// ICMP, UDP, DHCP client, DNS resolver and a client-side TCP. Everything is
// polled from the main loop; no interrupts, no threads.
#include "net/net.h"
#include "drivers/pci.h"
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "lib/heap.h"
#include "lib/string.h"
#include "lib/log.h"
#include "lib/vector.h"

namespace net {

namespace {

// ================================================================ helpers
inline uint16_t bswap16(uint16_t v) { return (v >> 8) | (v << 8); }
inline uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }
inline uint16_t rd16(const uint8_t* p) { return (p[0] << 8) | p[1]; }
inline uint32_t rd32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
inline void wr16(uint8_t* p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
inline void wr32(uint8_t* p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

uint32_t csum_add(uint32_t sum, const uint8_t* data, size_t len) {
    for (size_t i = 0; i + 1 < len; i += 2) sum += (data[i] << 8) | data[i + 1];
    if (len & 1) sum += data[len - 1] << 8;
    return sum;
}
uint16_t csum_fold(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

// ================================================================ e1000
enum : uint32_t {
    REG_CTRL = 0x0000, REG_STATUS = 0x0008, REG_EERD = 0x0014, REG_ICR = 0x00C0, REG_IMC = 0x00D8,
    REG_RCTL = 0x0100, REG_TCTL = 0x0400, REG_TIPG = 0x0410,
    REG_RDBAL = 0x2800, REG_RDBAH = 0x2804, REG_RDLEN = 0x2808, REG_RDH = 0x2810, REG_RDT = 0x2818,
    REG_TDBAL = 0x3800, REG_TDBAH = 0x3804, REG_TDLEN = 0x3808, REG_TDH = 0x3810, REG_TDT = 0x3818,
    REG_MTA = 0x5200, REG_RAL = 0x5400, REG_RAH = 0x5404,
};

struct RxDesc { uint64_t addr; uint16_t length, csum; uint8_t status, errors; uint16_t special; } __attribute__((packed));
struct TxDesc { uint64_t addr; uint16_t length; uint8_t cso, cmd, status, css; uint16_t special; } __attribute__((packed));

constexpr int kRxCount = 64, kTxCount = 32, kBufSize = 2048;

volatile uint8_t* g_mmio;
RxDesc* g_rx;
TxDesc* g_tx;
uint8_t* g_rx_bufs[kRxCount];
uint8_t* g_tx_bufs[kTxCount];
int g_rx_cur, g_tx_cur;
bool g_nic;
const char* g_nic_name = "none";
const NicDriver* g_ext_nic;    // from a .dri driver; replaces the e1000

uint8_t g_mac[6];
uint32_t g_ip, g_mask, g_gw, g_dns;
bool g_configured;
Status g_stats;
char g_ip_str[16];

inline uint32_t rreg(uint32_t r) { return *(volatile uint32_t*)(g_mmio + r); }
inline void wreg(uint32_t r, uint32_t v) { *(volatile uint32_t*)(g_mmio + r) = v; }

void* alloc_aligned(size_t size) {
    uint8_t* raw = (uint8_t*)kmalloc(size + 4096);
    uint8_t* p = (uint8_t*)(((uintptr_t)raw + 4095) & ~4095ull);
    memset(p, 0, size);
    return p;
}

bool e1000_init() {
    const PciDevice* d = pci_find(0x8086, 0x100E);   // 82540EM (QEMU default)
    if (!d) d = pci_find(0x8086, 0x100F);            // 82545EM
    if (!d) d = pci_find(0x8086, 0x10D3);            // 82574L (no-legacy fallback, best effort)
    if (!d) return false;
    pci_enable(d);
    g_mmio = (volatile uint8_t*)(uintptr_t)(d->bar[0] & ~0xFull);
    g_nic_name = d->device == 0x100E ? "Intel 82540EM Gigabit Ethernet" : "Intel PRO/1000 Ethernet";

    wreg(REG_IMC, 0xFFFFFFFF);
    wreg(REG_CTRL, rreg(REG_CTRL) | (1u << 26));      // reset
    for (int i = 0; i < 100000; i++) if (!(rreg(REG_CTRL) & (1u << 26))) break;
    wreg(REG_IMC, 0xFFFFFFFF);
    rreg(REG_ICR);
    wreg(REG_CTRL, (rreg(REG_CTRL) | (1u << 6)) & ~((1u << 3) | (1u << 31) | (1u << 7)));   // SLU, clear LRST/PHY_RST/ILOS

    uint32_t ral = rreg(REG_RAL), rah = rreg(REG_RAH);
    if (ral || (rah & 0xFFFF)) {
        for (int i = 0; i < 4; i++) g_mac[i] = ral >> (i * 8);
        g_mac[4] = rah;
        g_mac[5] = rah >> 8;
    } else {
        for (int i = 0; i < 3; i++) {
            wreg(REG_EERD, 1 | (i << 8));
            uint32_t v;
            for (int k = 0; k < 100000; k++) if ((v = rreg(REG_EERD)) & (1 << 4)) break;
            g_mac[i * 2] = v >> 16;
            g_mac[i * 2 + 1] = v >> 24;
        }
    }
    for (int i = 0; i < 128; i++) wreg(REG_MTA + i * 4, 0);

    g_rx = (RxDesc*)alloc_aligned(sizeof(RxDesc) * kRxCount);
    for (int i = 0; i < kRxCount; i++) {
        g_rx_bufs[i] = (uint8_t*)alloc_aligned(kBufSize);
        g_rx[i].addr = (uint64_t)(uintptr_t)g_rx_bufs[i];
    }
    wreg(REG_RDBAL, (uint32_t)(uintptr_t)g_rx);
    wreg(REG_RDBAH, 0);
    wreg(REG_RDLEN, sizeof(RxDesc) * kRxCount);
    wreg(REG_RDH, 0);
    wreg(REG_RDT, kRxCount - 1);
    wreg(REG_RCTL, (1u << 1) | (1u << 4) | (1u << 15) | (1u << 26));   // EN, MPE, BAM, SECRC, 2048 bytes

    g_tx = (TxDesc*)alloc_aligned(sizeof(TxDesc) * kTxCount);
    for (int i = 0; i < kTxCount; i++) {
        g_tx_bufs[i] = (uint8_t*)alloc_aligned(kBufSize);
        g_tx[i].addr = (uint64_t)(uintptr_t)g_tx_bufs[i];
        g_tx[i].status = 1;   // DD: free
    }
    wreg(REG_TDBAL, (uint32_t)(uintptr_t)g_tx);
    wreg(REG_TDBAH, 0);
    wreg(REG_TDLEN, sizeof(TxDesc) * kTxCount);
    wreg(REG_TDH, 0);
    wreg(REG_TDT, 0);
    wreg(REG_TCTL, (1u << 1) | (1u << 3) | (0x10u << 4) | (0x40u << 12));
    wreg(REG_TIPG, 0x0060200A);
    klog("net: %s, MAC %02x:%02x:%02x:%02x:%02x:%02x", g_nic_name, g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    return true;
}

bool link_up() { return g_nic && (g_ext_nic ? g_ext_nic->link_up() : (rreg(REG_STATUS) & 2)); }

void eth_send_raw(const uint8_t* frame, size_t len) {
    if (!g_nic || len > kBufSize) return;
    if (g_ext_nic) {
        g_ext_nic->send(frame, len);
        g_stats.tx_packets++;
        g_stats.tx_bytes += len;
        return;
    }
    TxDesc& d = g_tx[g_tx_cur];
    for (int i = 0; i < 100000 && !(d.status & 1); i++) cpu_relax();
    if (!(d.status & 1)) return;   // ring full, drop
    if (len < 60) { memset(g_tx_bufs[g_tx_cur] + len, 0, 60 - len); }
    memcpy(g_tx_bufs[g_tx_cur], frame, len);
    d.length = len < 60 ? 60 : len;
    d.cmd = 0x01 | 0x02 | 0x08;   // EOP | IFCS | RS
    d.status = 0;
    g_tx_cur = (g_tx_cur + 1) % kTxCount;
    wreg(REG_TDT, g_tx_cur);
    g_stats.tx_packets++;
    g_stats.tx_bytes += len;
}

// ================================================================ ethernet + ARP
const uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

void eth_send(const uint8_t* dst, uint16_t type, const uint8_t* payload, size_t len) {
    uint8_t frame[1600];
    if (len > 1500) return;
    memcpy(frame, dst, 6);
    memcpy(frame + 6, g_mac, 6);
    wr16(frame + 12, type);
    memcpy(frame + 14, payload, len);
    eth_send_raw(frame, 14 + len);
}

struct ArpEntry { uint32_t ip; uint8_t mac[6]; uint64_t time; };
ArpEntry g_arp[32];

struct PendingPacket { uint32_t next_hop; uint8_t data[1500]; size_t len; uint64_t time; bool used; };
PendingPacket g_pending[16];

ArpEntry* arp_lookup(uint32_t ip) {
    for (ArpEntry& e : g_arp) if (e.ip == ip && e.time) return &e;
    return nullptr;
}

void arp_store(uint32_t ip, const uint8_t* mac) {
    ArpEntry* slot = arp_lookup(ip);
    if (!slot) {
        slot = &g_arp[0];
        for (ArpEntry& e : g_arp) { if (!e.time) { slot = &e; break; } if (e.time < slot->time) slot = &e; }
    }
    slot->ip = ip;
    memcpy(slot->mac, mac, 6);
    slot->time = timer_ms() + 1;
}

void arp_send(uint16_t op, const uint8_t* tha, uint32_t tpa) {
    uint8_t p[28];
    wr16(p, 1);            // ethernet
    wr16(p + 2, 0x0800);   // ipv4
    p[4] = 6; p[5] = 4;
    wr16(p + 6, op);
    memcpy(p + 8, g_mac, 6);
    wr32(p + 14, g_ip);
    memcpy(p + 18, tha, 6);
    wr32(p + 24, tpa);
    eth_send(op == 1 ? kBroadcast : tha, 0x0806, p, 28);
}

void flush_pending(uint32_t ip, const uint8_t* mac) {
    for (PendingPacket& p : g_pending)
        if (p.used && p.next_hop == ip) {
            eth_send(mac, 0x0800, p.data, p.len);
            p.used = false;
        }
}

void arp_receive(const uint8_t* p, size_t len) {
    if (len < 28) return;
    uint16_t op = rd16(p + 6);
    uint32_t spa = rd32(p + 14), tpa = rd32(p + 24);
    if (spa) arp_store(spa, p + 8);
    flush_pending(spa, p + 8);
    if (op == 1 && g_configured && tpa == g_ip) arp_send(2, p + 8, spa);
}

// ================================================================ IPv4
uint16_t g_ip_id = 1;

void ip_send(uint32_t dst, uint8_t proto, const uint8_t* payload, size_t len) {
    if (len > 1480) return;
    uint8_t pkt[1500];
    pkt[0] = 0x45;
    pkt[1] = 0;
    wr16(pkt + 2, 20 + len);
    wr16(pkt + 4, g_ip_id++);
    wr16(pkt + 6, 0x4000);   // DF
    pkt[8] = 64;
    pkt[9] = proto;
    wr16(pkt + 10, 0);
    wr32(pkt + 12, g_ip);
    wr32(pkt + 16, dst);
    wr16(pkt + 10, csum_fold(csum_add(0, pkt, 20)));
    memcpy(pkt + 20, payload, len);
    size_t total = 20 + len;
    if (dst == 0xFFFFFFFF) { eth_send(kBroadcast, 0x0800, pkt, total); return; }
    uint32_t hop = (g_mask && (dst & g_mask) == (g_ip & g_mask)) ? dst : g_gw;
    ArpEntry* e = arp_lookup(hop);
    if (e) { eth_send(e->mac, 0x0800, pkt, total); return; }
    // queue and resolve
    for (PendingPacket& p : g_pending)
        if (!p.used) {
            p.used = true;
            p.next_hop = hop;
            memcpy(p.data, pkt, total);
            p.len = total;
            p.time = timer_ms();
            break;
        }
    static const uint8_t zero[6] = {0};
    arp_send(1, zero, hop);
}

// ================================================================ UDP
void udp_send(uint32_t dst, uint16_t sport, uint16_t dport, const uint8_t* data, size_t len) {
    uint8_t seg[1480];
    if (len > 1472) return;
    wr16(seg, sport);
    wr16(seg + 2, dport);
    wr16(seg + 4, 8 + len);
    wr16(seg + 6, 0);
    memcpy(seg + 8, data, len);
    uint8_t pseudo[12];
    wr32(pseudo, g_ip);
    wr32(pseudo + 4, dst);
    pseudo[8] = 0; pseudo[9] = 17;
    wr16(pseudo + 10, 8 + len);
    uint16_t c = csum_fold(csum_add(csum_add(0, pseudo, 12), seg, 8 + len));
    wr16(seg + 6, c ? c : 0xFFFF);
    ip_send(dst, 17, seg, 8 + len);
}

// ================================================================ DHCP
enum class DhcpState { Idle, Discovering, Requesting, Bound };
DhcpState g_dhcp = DhcpState::Idle;
uint32_t g_dhcp_xid, g_dhcp_offer, g_dhcp_server;
uint64_t g_dhcp_time;

void dhcp_send(uint8_t type) {
    uint8_t p[300];
    memset(p, 0, sizeof(p));
    p[0] = 1; p[1] = 1; p[2] = 6;
    wr32(p + 4, g_dhcp_xid);
    wr16(p + 10, 0x8000);   // broadcast flag
    memcpy(p + 28, g_mac, 6);
    wr32(p + 236, 0x63825363);
    int o = 240;
    p[o++] = 53; p[o++] = 1; p[o++] = type;
    p[o++] = 61; p[o++] = 7; p[o++] = 1; memcpy(p + o, g_mac, 6); o += 6;
    p[o++] = 12; p[o++] = 7; memcpy(p + o, "kleinos", 7); o += 7;
    if (type == 3) {
        p[o++] = 50; p[o++] = 4; wr32(p + o, g_dhcp_offer); o += 4;
        p[o++] = 54; p[o++] = 4; wr32(p + o, g_dhcp_server); o += 4;
    }
    p[o++] = 55; p[o++] = 3; p[o++] = 1; p[o++] = 3; p[o++] = 6;
    p[o++] = 255;
    uint32_t saved = g_ip;
    g_ip = 0;
    udp_send(0xFFFFFFFF, 68, 67, p, o < 300 ? 300 : o);
    g_ip = saved;
    g_dhcp_time = timer_ms();
}

void dhcp_receive(const uint8_t* p, size_t len) {
    if (len < 240 || p[0] != 2 || rd32(p + 4) != g_dhcp_xid || rd32(p + 236) != 0x63825363) return;
    uint32_t yiaddr = rd32(p + 16);
    uint8_t type = 0;
    uint32_t mask = 0, router = 0, dns = 0, server = 0;
    for (size_t o = 240; o < len;) {
        uint8_t opt = p[o];
        if (opt == 255) break;
        if (opt == 0) { o++; continue; }
        if (o + 1 >= len) break;
        uint8_t l = p[o + 1];
        const uint8_t* v = p + o + 2;
        if (o + 2 + l > len) break;
        switch (opt) {
        case 53: type = v[0]; break;
        case 1: mask = rd32(v); break;
        case 3: router = rd32(v); break;
        case 6: dns = rd32(v); break;
        case 54: server = rd32(v); break;
        }
        o += 2 + l;
    }
    if (type == 2 && g_dhcp == DhcpState::Discovering) {
        g_dhcp_offer = yiaddr;
        g_dhcp_server = server;
        g_dhcp = DhcpState::Requesting;
        dhcp_send(3);
    } else if (type == 5 && g_dhcp == DhcpState::Requesting) {
        g_ip = yiaddr;
        g_mask = mask ? mask : 0xFFFFFF00;
        g_gw = router;
        g_dns = dns ? dns : router;
        g_configured = true;
        g_dhcp = DhcpState::Bound;
        ip_to_string(g_ip, g_ip_str);
        char gw[16], d[16];
        ip_to_string(g_gw, gw);
        ip_to_string(g_dns, d);
        klog("net: DHCP bound %s gw %s dns %s", g_ip_str, gw, d);
    } else if (type == 6) {
        g_dhcp = DhcpState::Discovering;
        dhcp_send(1);
    }
}

// ================================================================ DNS
struct DnsQuery {
    bool used;
    char host[128];
    uint16_t id, port;
    int state;       // 0 pending, 1 done, -1 failed
    uint32_t ip;
    uint64_t sent;
    int tries;
};
DnsQuery g_dns_q[16];
struct DnsCache { char host[128]; uint32_t ip; uint64_t time; };
DnsCache g_dns_cache[32];

void dns_send(DnsQuery& q) {
    uint8_t p[300];
    memset(p, 0, 12);
    wr16(p, q.id);
    wr16(p + 2, 0x0100);   // recursion desired
    wr16(p + 4, 1);
    int o = 12;
    const char* h = q.host;
    while (*h) {
        const char* dot = strchr(h, '.');
        int l = dot ? (int)(dot - h) : (int)strlen(h);
        if (l > 63 || o + l + 6 > 290) break;
        p[o++] = l;
        memcpy(p + o, h, l);
        o += l;
        h += l;
        if (*h == '.') h++;
    }
    p[o++] = 0;
    wr16(p + o, 1); o += 2;   // A
    wr16(p + o, 1); o += 2;   // IN
    udp_send(g_dns, q.port, 53, p, o);
    q.sent = timer_ms();
    q.tries++;
}

size_t dns_skip_name(const uint8_t* p, size_t len, size_t o) {
    while (o < len) {
        uint8_t l = p[o];
        if (l == 0) return o + 1;
        if ((l & 0xC0) == 0xC0) return o + 2;
        o += 1 + l;
    }
    return len;
}

void dns_receive(uint16_t port, const uint8_t* p, size_t len) {
    if (len < 12) return;
    uint16_t id = rd16(p);
    for (DnsQuery& q : g_dns_q) {
        if (!q.used || q.state != 0 || q.id != id || q.port != port) continue;
        uint16_t flags = rd16(p + 2);
        int qd = rd16(p + 4), an = rd16(p + 6);
        if ((flags & 0xF) != 0 || an == 0) { q.state = -1; return; }
        size_t o = 12;
        for (int i = 0; i < qd; i++) o = dns_skip_name(p, len, o) + 4;
        for (int i = 0; i < an && o < len; i++) {
            o = dns_skip_name(p, len, o);
            if (o + 10 > len) break;
            uint16_t type = rd16(p + o), rdlen = rd16(p + o + 8);
            o += 10;
            if (type == 1 && rdlen == 4 && o + 4 <= len) {
                q.ip = rd32(p + o);
                q.state = 1;
                DnsCache* slot = &g_dns_cache[0];
                for (DnsCache& c : g_dns_cache) { if (!c.time) { slot = &c; break; } if (c.time < slot->time) slot = &c; }
                strlcpy(slot->host, q.host, sizeof(slot->host));
                slot->ip = q.ip;
                slot->time = timer_ms() + 1;
                return;
            }
            o += rdlen;
        }
        q.state = -1;
        return;
    }
}

// ================================================================ ICMP
struct Ping { uint16_t seq; uint64_t sent; int rtt; bool used; };
Ping g_pings[32];
uint16_t g_ping_seq = 1;

void icmp_receive(uint32_t src, const uint8_t* p, size_t len) {
    if (len < 8) return;
    if (p[0] == 8) {   // echo request -> reply
        uint8_t r[1480];
        if (len > sizeof(r)) return;
        memcpy(r, p, len);
        r[0] = 0;
        wr16(r + 2, 0);
        wr16(r + 2, csum_fold(csum_add(0, r, len)));
        ip_send(src, 1, r, len);
    } else if (p[0] == 0) {
        uint16_t seq = rd16(p + 6);
        for (Ping& pg : g_pings)
            if (pg.used && pg.seq == seq && pg.rtt < 0) pg.rtt = (int)(timer_ms() - pg.sent);
    }
}

// ================================================================ TCP
enum class TS { Closed, SynSent, Established, FinWait1, FinWait2, CloseWait, LastAck, Error };

struct Tcp {
    bool used;
    TS state;
    uint32_t rip;
    uint16_t lport, rport;
    uint32_t iss, snd_una, snd_nxt, snd_wnd;
    uint32_t irs, rcv_nxt;
    uint16_t mss;
    // send buffer: bytes from snd_una onwards (unacked + unsent)
    uint8_t* sbuf;
    size_t slen, scap;
    // receive ring
    uint8_t* rbuf;
    size_t rhead, rlen, rcap;
    bool fin_received, fin_sent, close_requested;
    uint64_t last_tx, rto;
    int retries;
    uint64_t last_ack_sent;
};
constexpr int kMaxTcp = 32;
Tcp g_tcp[kMaxTcp];
uint16_t g_next_port = 49152;

void tcp_segment(Tcp& t, uint8_t flags, uint32_t seq, const uint8_t* data, size_t len) {
    uint8_t seg[1480];
    int hl = 20 + ((flags & 0x02) ? 4 : 0);
    wr16(seg, t.lport);
    wr16(seg + 2, t.rport);
    wr32(seg + 4, seq);
    wr32(seg + 8, (flags & 0x10) ? t.rcv_nxt : 0);
    seg[12] = (hl / 4) << 4;
    seg[13] = flags;
    size_t free = t.rcap - t.rlen;
    wr16(seg + 14, free > 65535 ? 65535 : (uint16_t)free);
    wr16(seg + 16, 0);
    wr16(seg + 18, 0);
    if (flags & 0x02) { seg[20] = 2; seg[21] = 4; wr16(seg + 22, 1460); }
    if (len) memcpy(seg + hl, data, len);
    uint8_t pseudo[12];
    wr32(pseudo, g_ip);
    wr32(pseudo + 4, t.rip);
    pseudo[8] = 0; pseudo[9] = 6;
    wr16(pseudo + 10, hl + len);
    wr16(seg + 16, csum_fold(csum_add(csum_add(0, pseudo, 12), seg, hl + len)));
    ip_send(t.rip, 6, seg, hl + len);
    if (flags & 0x10) t.last_ack_sent = timer_ms();
}

void tcp_send_rst(uint32_t rip, uint16_t lport, uint16_t rport, uint32_t seq, uint32_t ack) {
    Tcp t{};
    t.rip = rip; t.lport = lport; t.rport = rport; t.rcv_nxt = ack; t.rcap = 0;
    tcp_segment(t, 0x14, seq, nullptr, 0);
}

// Transmit as much unsent data as the window allows.
void tcp_output(Tcp& t) {
    if (t.state != TS::Established && t.state != TS::CloseWait) return;
    size_t inflight = t.snd_nxt - t.snd_una;
    while (inflight < t.slen) {
        size_t wnd = t.snd_wnd ? t.snd_wnd : 1;
        if (inflight >= wnd) break;
        size_t n = t.slen - inflight;
        if (n > t.mss) n = t.mss;
        if (n > wnd - inflight) n = wnd - inflight;
        tcp_segment(t, 0x18, t.snd_nxt, t.sbuf + inflight, n);   // PSH|ACK
        t.snd_nxt += n;
        inflight += n;
        t.last_tx = timer_ms();
    }
    if (t.close_requested && !t.fin_sent && t.snd_nxt - t.snd_una == t.slen) {
        tcp_segment(t, 0x11, t.snd_nxt, nullptr, 0);   // FIN|ACK
        t.snd_nxt++;
        t.fin_sent = true;
        t.last_tx = timer_ms();
        t.state = t.state == TS::CloseWait ? TS::LastAck : TS::FinWait1;
    }
}

void tcp_free(Tcp& t) {
    kfree(t.sbuf);
    kfree(t.rbuf);
    t = Tcp{};
}

void tcp_receive(uint32_t src, const uint8_t* p, size_t len) {
    if (len < 20) return;
    uint16_t sport = rd16(p), dport = rd16(p + 2);
    uint32_t seq = rd32(p + 4), ack = rd32(p + 8);
    int hl = (p[12] >> 4) * 4;
    uint8_t flags = p[13];
    uint16_t wnd = rd16(p + 14);
    if (hl < 20 || (size_t)hl > len) return;
    const uint8_t* data = p + hl;
    size_t dlen = len - hl;

    Tcp* tp = nullptr;
    for (Tcp& t : g_tcp)
        if (t.used && t.lport == dport && t.rport == sport && t.rip == src) { tp = &t; break; }
    if (!tp) {
        if (!(flags & 0x04)) tcp_send_rst(src, dport, sport, (flags & 0x10) ? ack : 0, seq + dlen + ((flags & 0x03) ? 1 : 0));
        return;
    }
    Tcp& t = *tp;
    if (flags & 0x04) {   // RST
        t.state = t.state == TS::SynSent ? TS::Error : TS::Closed;
        if (t.state == TS::Closed) t.fin_received = true;
        return;
    }
    if (t.state == TS::SynSent) {
        if ((flags & 0x12) == 0x12 && ack == t.iss + 1) {
            t.irs = seq;
            t.rcv_nxt = seq + 1;
            t.snd_una = ack;
            t.snd_wnd = wnd;
            // MSS option
            for (int o = 20; o + 1 < hl;) {
                uint8_t kind = p[o];
                if (kind == 0) break;
                if (kind == 1) { o++; continue; }
                uint8_t l = p[o + 1];
                if (kind == 2 && l == 4 && o + 4 <= hl) { uint16_t m = rd16(p + o + 2); if (m < t.mss) t.mss = m; }
                if (l < 2) break;
                o += l;
            }
            t.state = TS::Established;
            t.retries = 0;
            tcp_segment(t, 0x10, t.snd_nxt, nullptr, 0);
            tcp_output(t);
        }
        return;
    }
    // ACK processing
    if (flags & 0x10) {
        int32_t acked = (int32_t)(ack - t.snd_una);
        int32_t ahead = (int32_t)(ack - t.snd_nxt);
        if (acked > 0 && ahead <= 0) {
            size_t data_acked = (size_t)acked;
            if (t.fin_sent && ack == t.snd_nxt) data_acked = acked - 1 <= (int32_t)t.slen ? acked - 1 : t.slen;
            if (data_acked > t.slen) data_acked = t.slen;
            memmove(t.sbuf, t.sbuf + data_acked, t.slen - data_acked);
            t.slen -= data_acked;
            t.snd_una = ack;
            t.retries = 0;
            t.rto = 400;
            t.last_tx = timer_ms();
            if (t.fin_sent && ack == t.snd_nxt) {
                if (t.state == TS::FinWait1) t.state = TS::FinWait2;
                else if (t.state == TS::LastAck) { t.state = TS::Closed; }
            }
        }
        t.snd_wnd = wnd;
    }
    // data
    bool need_ack = false;
    if (dlen > 0) {
        if (seq == t.rcv_nxt) {
            size_t space = t.rcap - t.rlen;
            size_t n = dlen < space ? dlen : space;
            for (size_t i = 0; i < n; i++) t.rbuf[(t.rhead + t.rlen + i) % t.rcap] = data[i];
            t.rlen += n;
            t.rcv_nxt += n;
        }
        need_ack = true;   // in-order or duplicate/out-of-order -> (dup) ACK
    }
    if ((flags & 0x01) && seq + dlen == t.rcv_nxt && !t.fin_received) {
        t.rcv_nxt++;
        t.fin_received = true;
        need_ack = true;
        if (t.state == TS::Established) t.state = TS::CloseWait;
        else if (t.state == TS::FinWait1 || t.state == TS::FinWait2) t.state = TS::Closed;
    }
    if (need_ack) tcp_segment(t, 0x10, t.snd_nxt, nullptr, 0);
    tcp_output(t);
}

void tcp_timers() {
    uint64_t now = timer_ms();
    for (Tcp& t : g_tcp) {
        if (!t.used) continue;
        bool outstanding = t.state == TS::SynSent || t.snd_nxt != t.snd_una;
        if (outstanding && now - t.last_tx > t.rto) {
            if (++t.retries > 8) { t.state = TS::Error; continue; }
            t.rto = t.rto * 2 > 8000 ? 8000 : t.rto * 2;
            t.last_tx = now;
            if (t.state == TS::SynSent) {
                tcp_segment(t, 0x02, t.iss, nullptr, 0);
            } else {
                // go-back-N: resend from snd_una
                bool fin_pending = t.fin_sent && t.snd_nxt - t.snd_una == t.slen + 1;
                t.snd_nxt = t.snd_una;
                if (fin_pending) t.fin_sent = false;
                if (t.state == TS::FinWait1) t.state = TS::Established;
                if (t.state == TS::LastAck) t.state = TS::CloseWait;
                tcp_output(t);
            }
        }
        // window update when the app drained data
        if ((t.state == TS::Established || t.state == TS::FinWait2) && now - t.last_ack_sent > 200 && t.rlen < t.rcap / 2)
            tcp_segment(t, 0x10, t.snd_nxt, nullptr, 0);
    }
}

// ================================================================ dispatch
void ip_receive(const uint8_t* p, size_t len) {
    if (len < 20 || (p[0] >> 4) != 4) return;
    int ihl = (p[0] & 0xF) * 4;
    uint16_t total = rd16(p + 2);
    if (total < ihl || total > len) return;
    uint32_t src = rd32(p + 12), dst = rd32(p + 16);
    if (g_configured && dst != g_ip && dst != 0xFFFFFFFF && (dst | g_mask) != 0xFFFFFFFF) return;
    uint16_t frag = rd16(p + 6);
    if ((frag & 0x1FFF) || (frag & 0x2000)) return;   // no fragment reassembly
    const uint8_t* pl = p + ihl;
    size_t plen = total - ihl;
    switch (p[9]) {
    case 1: icmp_receive(src, pl, plen); break;
    case 6: if (g_configured) tcp_receive(src, pl, plen); break;
    case 17: {
        if (plen < 8) return;
        uint16_t dport = rd16(pl + 2);
        uint16_t ulen = rd16(pl + 4);
        if (ulen < 8 || ulen > plen) return;
        if (dport == 68) dhcp_receive(pl + 8, ulen - 8);
        else dns_receive(dport, pl + 8, ulen - 8);
        break;
    }
    }
}

void rx_frame(const uint8_t* f, size_t len) {
    if (len < 14) return;
    g_stats.rx_packets++;
    g_stats.rx_bytes += len;
    uint16_t type = rd16(f + 12);
    if (type == 0x0806) arp_receive(f + 14, len - 14);
    else if (type == 0x0800) ip_receive(f + 14, len - 14);
}

void rx_poll() {
    if (g_ext_nic) { g_ext_nic->poll(rx_frame); return; }
    for (int budget = 0; budget < 64; budget++) {
        RxDesc& d = g_rx[g_rx_cur];
        if (!(d.status & 1)) break;
        uint16_t len = d.length;
        uint8_t* f = g_rx_bufs[g_rx_cur];
        if (!d.errors) rx_frame(f, len);
        d.status = 0;
        int old = g_rx_cur;
        g_rx_cur = (g_rx_cur + 1) % kRxCount;
        wreg(REG_RDT, old);
    }
}

}  // namespace

// ================================================================ public API
void register_nic(const NicDriver* nic) {
    g_ext_nic = nic;
    g_nic_name = nic->name;
    memcpy(g_mac, nic->mac, 6);
    klog("net: using driver NIC %s", nic->name);
}

void init() {
    g_nic = g_ext_nic ? true : e1000_init();
    if (!g_nic) { klog("net: no supported network card"); return; }
    g_dhcp_xid = (uint32_t)cpu_random64();
    g_dhcp = DhcpState::Discovering;
    dhcp_send(1);
}

void poll() {
    if (!g_nic) return;
    rx_poll();
    uint64_t now = timer_ms();
    if ((g_dhcp == DhcpState::Discovering || g_dhcp == DhcpState::Requesting) && now - g_dhcp_time > 2000) {
        g_dhcp = DhcpState::Discovering;
        dhcp_send(1);
    }
    for (DnsQuery& q : g_dns_q)
        if (q.used && q.state == 0 && now - q.sent > 1500) {
            if (q.tries >= 4) q.state = -1;
            else if (g_configured) dns_send(q);
        }
    for (PendingPacket& p : g_pending)
        if (p.used && now - p.time > 3000) p.used = false;
    for (Ping& pg : g_pings)
        if (pg.used && pg.rtt < 0 && now - pg.sent > 4000) pg.used = false;
    tcp_timers();
}

bool is_up() { return g_configured && link_up(); }
const char* ip_string() { return g_configured ? g_ip_str : ""; }
const char* nic_name() { return g_nic_name; }

Status status() {
    Status s = g_stats;
    s.link = link_up();
    s.configured = g_configured;
    s.ip = g_ip; s.netmask = g_mask; s.gateway = g_gw; s.dns = g_dns;
    memcpy(s.mac, g_mac, 6);
    return s;
}

void ip_to_string(uint32_t ip, char* out) {
    snprintf(out, 16, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}

static bool parse_ip(const char* s, uint32_t* out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        if (!isdigit(*s)) return false;
        int n = 0;
        while (isdigit(*s)) n = n * 10 + (*s++ - '0');
        if (n > 255) return false;
        v = (v << 8) | n;
        if (i < 3) { if (*s != '.') return false; s++; }
    }
    if (*s) return false;
    *out = v;
    return true;
}

int dns_resolve(const char* host) {
    for (int i = 0; i < 16; i++) {
        DnsQuery& q = g_dns_q[i];
        if (q.used && q.state != 0) continue;   // finished: reusable only after caller read it
        if (q.used) continue;
        q = DnsQuery{};
        q.used = true;
        strlcpy(q.host, host, sizeof(q.host));
        uint32_t lit;
        if (parse_ip(host, &lit)) { q.ip = lit; q.state = 1; return i + 1; }
        if (strcmp(host, "localhost") == 0) { q.ip = 0x7F000001; q.state = 1; return i + 1; }
        for (DnsCache& c : g_dns_cache)
            if (c.time && strcasecmp(c.host, host) == 0 && timer_ms() - c.time < 300000) { q.ip = c.ip; q.state = 1; return i + 1; }
        q.id = (uint16_t)cpu_random64();
        q.port = g_next_port++;
        if (g_next_port > 65000) g_next_port = 49152;
        if (!g_configured) { q.state = -1; return i + 1; }
        dns_send(q);
        return i + 1;
    }
    return -1;
}

int dns_result(int h, uint32_t* ip) {
    if (h < 1 || h > 16) return -1;
    DnsQuery& q = g_dns_q[h - 1];
    if (!q.used) return -1;
    if (q.state == 0) return 0;
    int r = q.state;
    if (r == 1 && ip) *ip = q.ip;
    q.used = false;   // released once the result has been read
    return r;
}

int ping_send(uint32_t ip) {
    if (!g_configured) return -1;
    Ping* slot = nullptr;
    for (Ping& p : g_pings) if (!p.used) { slot = &p; break; }
    if (!slot) return -1;
    uint16_t seq = g_ping_seq++;
    *slot = Ping{seq, timer_ms(), -1, true};
    uint8_t p[64];
    memset(p, 0, sizeof(p));
    p[0] = 8;
    wr16(p + 4, 0x4B4C);   // id "KL"
    wr16(p + 6, seq);
    for (int i = 8; i < 64; i++) p[i] = (uint8_t)i;
    wr16(p + 2, csum_fold(csum_add(0, p, 64)));
    ip_send(ip, 1, p, 64);
    return seq;
}

int ping_result(int seq) {
    for (Ping& p : g_pings)
        if (p.used && p.seq == seq) {
            if (p.rtt >= 0) { int r = p.rtt; p.used = false; return r; }
            return -1;
        }
    return -2;   // unknown / timed out
}

int tcp_connect(uint32_t ip, uint16_t port) {
    if (!g_configured) return -1;
    for (int i = 0; i < kMaxTcp; i++) {
        Tcp& t = g_tcp[i];
        if (t.used) continue;
        t = Tcp{};
        t.used = true;
        t.state = TS::SynSent;
        t.rip = ip;
        t.rport = port;
        t.lport = g_next_port++;
        if (g_next_port > 65000) g_next_port = 49152;
        t.iss = (uint32_t)cpu_random64();
        t.snd_una = t.iss;
        t.snd_nxt = t.iss + 1;
        t.mss = 1460;
        t.scap = 16384;
        t.sbuf = (uint8_t*)kmalloc(t.scap);
        t.rcap = 256 * 1024;
        t.rbuf = (uint8_t*)kmalloc(t.rcap);
        t.rto = 600;
        t.last_tx = timer_ms();
        tcp_segment(t, 0x02, t.iss, nullptr, 0);
        return i;
    }
    return -1;
}

TcpState tcp_state(int s) {
    if (s < 0 || s >= kMaxTcp || !g_tcp[s].used) return TcpState::Error;
    switch (g_tcp[s].state) {
    case TS::SynSent: return TcpState::Connecting;
    case TS::Established: case TS::FinWait1: case TS::FinWait2: case TS::CloseWait: case TS::LastAck: return TcpState::Open;
    case TS::Error: return TcpState::Error;
    default: return TcpState::Closed;
    }
}

int tcp_send(int s, const void* data, size_t len) {
    if (s < 0 || s >= kMaxTcp || !g_tcp[s].used) return -1;
    Tcp& t = g_tcp[s];
    if (t.state != TS::Established && t.state != TS::CloseWait) return t.state == TS::SynSent ? 0 : -1;
    if (t.slen + len > t.scap) {
        size_t nc = t.scap;
        while (nc < t.slen + len) nc *= 2;
        if (nc > 4 * 1024 * 1024) return -1;
        uint8_t* nb = (uint8_t*)kmalloc(nc);
        memcpy(nb, t.sbuf, t.slen);
        kfree(t.sbuf);
        t.sbuf = nb;
        t.scap = nc;
    }
    memcpy(t.sbuf + t.slen, data, len);
    t.slen += len;
    tcp_output(t);
    return (int)len;
}

size_t tcp_available(int s) {
    if (s < 0 || s >= kMaxTcp || !g_tcp[s].used) return 0;
    return g_tcp[s].rlen;
}

size_t tcp_recv(int s, void* buf, size_t max) {
    if (s < 0 || s >= kMaxTcp || !g_tcp[s].used) return 0;
    Tcp& t = g_tcp[s];
    size_t n = t.rlen < max ? t.rlen : max;
    uint8_t* o = (uint8_t*)buf;
    for (size_t i = 0; i < n; i++) o[i] = t.rbuf[(t.rhead + i) % t.rcap];
    t.rhead = (t.rhead + n) % t.rcap;
    bool was_full = t.rlen > t.rcap / 2;
    t.rlen -= n;
    if (was_full && t.rlen <= t.rcap / 2 && t.state == TS::Established) tcp_segment(t, 0x10, t.snd_nxt, nullptr, 0);
    return n;
}

bool tcp_peer_closed(int s) {
    if (s < 0 || s >= kMaxTcp || !g_tcp[s].used) return true;
    Tcp& t = g_tcp[s];
    return (t.fin_received || t.state == TS::Closed || t.state == TS::Error) && t.rlen == 0;
}

void tcp_close(int s) {
    if (s < 0 || s >= kMaxTcp || !g_tcp[s].used) return;
    Tcp& t = g_tcp[s];
    if (t.state == TS::Established || t.state == TS::CloseWait) {
        t.close_requested = true;
        tcp_output(t);
        // Fire and forget: the slot is released immediately; late segments get RST.
        tcp_send_rst(t.rip, t.lport, t.rport, t.snd_nxt, t.rcv_nxt);
    }
    tcp_free(t);
}

}  // namespace net
