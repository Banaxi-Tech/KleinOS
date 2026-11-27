// Intel High Definition Audio: the sound chip of nearly every PC since 2004 (and of
// QEMU's intel-hda). One output stream, 48 kHz 16-bit stereo, played from a ring
// buffer that the kernel mixer (kernel/drivers/sound.cpp) keeps filled ahead of
// the hardware. The codec is set up generically: every speaker, headphone and line
// output pin gets a path to a converter; the speakers go quiet while headphones
// are plugged in.
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "drivers/pci.h"
#include "drivers/sound.h"
#include "lib/heap.h"
#include "lib/log.h"
#include "lib/string.h"
#include "sys/driver.h"
#include "sys/system.h"

namespace {

// controller registers
constexpr uint32_t GCAP = 0x00, GCTL = 0x08, STATESTS = 0x0E, INTCTL = 0x20;
constexpr uint32_t CORBLBASE = 0x40, CORBUBASE = 0x44, CORBWP = 0x48, CORBRP = 0x4A, CORBCTL = 0x4C, CORBSIZE = 0x4E;
constexpr uint32_t RIRBLBASE = 0x50, RIRBUBASE = 0x54, RIRBWP = 0x58, RINTCNT = 0x5A, RIRBCTL = 0x5C, RIRBSTS = 0x5D, RIRBSIZE = 0x5E;
// stream descriptor registers (offsets from the descriptor)
constexpr uint32_t SD_CTL = 0x00, SD_STS = 0x03, SD_LPIB = 0x04, SD_CBL = 0x08, SD_LVI = 0x0C, SD_FMT = 0x12, SD_BDPL = 0x18, SD_BDPU = 0x1C;

constexpr uint16_t kFormat = 0x0011;   // 48 kHz, 16 bits, 2 channels
constexpr uint8_t kStreamTag = 1;
constexpr uint32_t kRingFrames = 4096;            // 85 ms
constexpr uint32_t kBdlEntries = 4;
constexpr uint32_t kLeadFrames = 1200;            // keep 25 ms written ahead of the hardware

volatile uint8_t* g_mmio;
uint32_t g_sd;                 // output stream descriptor offset
uint32_t* g_corb;
volatile uint64_t* g_rirb;
uint32_t g_corb_n, g_rirb_n, g_rirb_rp;
int16_t* g_ring;
uint32_t g_write;              // next frame to fill
int g_codec = -1;

uint8_t r8(uint32_t o) { return *(volatile uint8_t*)(g_mmio + o); }
uint16_t r16(uint32_t o) { return *(volatile uint16_t*)(g_mmio + o); }
uint32_t r32(uint32_t o) { return *(volatile uint32_t*)(g_mmio + o); }
void w8(uint32_t o, uint8_t v) { *(volatile uint8_t*)(g_mmio + o) = v; }
void w16(uint32_t o, uint16_t v) { *(volatile uint16_t*)(g_mmio + o) = v; }
void w32(uint32_t o, uint32_t v) { *(volatile uint32_t*)(g_mmio + o) = v; }

void delay_us(uint32_t us) { while (us--) io_wait(); }   // port 0x80: about 1 us each

template <typename F> bool wait_for(F cond, uint32_t us) {
    for (uint32_t i = 0; i < us / 10 + 1; i++) {
        if (cond()) return true;
        delay_us(10);
    }
    return cond();
}

void* dma_alloc(size_t size) {   // 128-byte aligned, zeroed; the heap is identity mapped
    uint8_t* p = (uint8_t*)kmalloc(size + 128);
    if (!p) return nullptr;
    p = (uint8_t*)(((uintptr_t)p + 127) & ~(uintptr_t)127);
    memset(p, 0, size);
    return p;
}

void set_base(uint32_t lo_reg, uint32_t hi_reg, const void* p) {
    uint64_t a = (uint64_t)(uintptr_t)p;
    w32(lo_reg, (uint32_t)a);
    w32(hi_reg, (uint32_t)(a >> 32));
}

// ---------------------------------------------------------------- codec commands
// verb: 12-bit verb with 8-bit payload, or 4-bit verb with 16-bit payload (20 bits).
bool g_cmd_broken;
uint32_t cmd(int nid, uint32_t verb) {
    if (g_cmd_broken) return 0;
    uint32_t wp = (r16(CORBWP) + 1) % g_corb_n;
    g_corb[wp] = (uint32_t)g_codec << 28 | (uint32_t)(nid & 0x7F) << 20 | (verb & 0xFFFFF);
    w16(CORBWP, (uint16_t)wp);
    uint32_t want = (g_rirb_rp + 1) % g_rirb_n;
    if (!wait_for([&] { return (r16(RIRBWP) & 0xFF) == want; }, 20000)) {
        klog("hda: codec %d did not answer verb %05x on node %d", g_codec, verb, nid);
        g_cmd_broken = true;
        return 0;
    }
    g_rirb_rp = want;
    w8(RIRBSTS, 5);   // acknowledge: some controllers stop answering after RINTCNT responses until this
    return (uint32_t)g_rirb[want];
}
uint32_t param(int nid, uint32_t p) { return cmd(nid, 0xF0000 | p); }

enum WType : uint8_t { W_OUT = 0, W_IN = 1, W_MIX = 2, W_SEL = 3, W_PIN = 4, W_POWER = 5, W_KNOB = 6, W_BEEP = 7 };
enum PinKind : uint8_t { P_NONE, P_LINE, P_SPEAKER, P_HP };

struct Widget {
    uint8_t type;
    uint32_t caps, pincaps, config, outamp, inamp;
    uint8_t conn[32];
    int nconn;
};

constexpr int kMaxNodes = 128;
Widget g_w[kMaxNodes];
int g_first, g_count;          // widget node range in the audio function group
int g_afg;

struct Output {
    uint8_t pin;
    PinKind kind;
    uint8_t path[8];           // pin first, converter last
    uint8_t pick[8];           // connection index chosen at path[i]
    int len;
    bool enabled;
};
Output g_out[8];
int g_nout;

bool valid(int nid) { return nid >= g_first && nid < g_first + g_count && nid < kMaxNodes; }

void read_connections(int nid) {
    Widget& w = g_w[nid];
    if (!(w.caps & (1 << 8))) return;
    uint32_t cl = param(nid, 0x0E);
    bool lng = cl & 0x80;
    int n = (int)(cl & 0x7F);
    int per = lng ? 2 : 4, prev = 0;
    for (int i = 0; i < n; i += per) {
        uint32_t r = cmd(nid, 0xF0200 | (uint32_t)i);
        for (int k = 0; k < per && i + k < n; k++) {
            uint32_t e = lng ? (r >> (16 * k)) & 0xFFFF : (r >> (8 * k)) & 0xFF;
            bool range = lng ? (e & 0x8000) : (e & 0x80);
            int id = (int)(e & (lng ? 0x7FFF : 0x7F));
            if (range && prev) {   // "prev..id"
                for (int x = prev + 1; x <= id && w.nconn < 32; x++) w.conn[w.nconn++] = (uint8_t)x;
            } else if (w.nconn < 32) {
                w.conn[w.nconn++] = (uint8_t)id;
            }
            prev = id;
        }
    }
}

bool find_path(Output& o, int nid, int depth) {
    if (!valid(nid) || depth >= 8) return false;
    for (int i = 0; i < depth; i++) if (o.path[i] == nid) return false;   // loops
    Widget& w = g_w[nid];
    o.path[depth] = (uint8_t)nid;
    if (w.type == W_OUT) { o.len = depth + 1; return true; }
    if (w.type != W_PIN && w.type != W_MIX && w.type != W_SEL) return false;
    if (w.type == W_PIN && depth) return false;
    for (int i = 0; i < w.nconn; i++) {
        o.pick[depth] = (uint8_t)i;
        if (find_path(o, w.conn[i], depth + 1)) return true;
    }
    return false;
}

uint32_t amp_caps(int nid, bool out) {
    Widget& w = g_w[nid];
    return (w.caps & (1 << 3)) ? (out ? w.outamp : w.inamp) : (out ? g_w[0].outamp : g_w[0].inamp);
}
uint32_t zero_db(uint32_t caps) {   // the 0 dB step, or the loudest if 0 dB is out of range
    uint32_t steps = (caps >> 8) & 0x7F, offset = caps & 0x7F;
    return offset <= steps ? offset : steps;
}

void set_pin(const Output& o, bool on) {
    cmd(o.pin, 0x70700 | (on ? (0x40u | (o.kind == P_HP ? 0x80u : 0)) : 0u));
}

void setup_output(Output& o) {
    for (int i = 0; i < o.len; i++) {
        int nid = o.path[i];
        Widget& w = g_w[nid];
        if (w.caps & (1 << 10)) cmd(nid, 0x70500);                       // power state D0
        if (w.caps & (1 << 2)) cmd(nid, 0x3B000 | zero_db(amp_caps(nid, true)));   // out amp: L+R, unmuted
        if (i + 1 < o.len && w.type != W_PIN && (w.caps & (1 << 1))) {  // in amps: only our input unmuted
            uint32_t g = zero_db(amp_caps(nid, false));
            int n = w.type == W_MIX ? w.nconn : 1;
            for (int k = 0; k < n; k++) {
                int idx = w.type == W_MIX ? k : o.pick[i];
                cmd(nid, 0x37000 | (uint32_t)idx << 8 | (idx == o.pick[i] ? g : 0x80));
            }
        }
        if (i + 1 < o.len && w.type != W_MIX && w.nconn > 1) cmd(nid, 0x70100 | o.pick[i]);   // connection select
        if (w.type == W_OUT) {
            cmd(nid, 0x20000 | kFormat);
            cmd(nid, 0x70600 | kStreamTag << 4);                          // stream 1, channel 0
        }
    }
    Widget& p = g_w[o.pin];
    if (p.pincaps & (1 << 16)) cmd(o.pin, 0x70C02);                       // external amplifier on
    set_pin(o, true);
    o.enabled = true;
}

bool hp_present() {
    for (int i = 0; i < g_nout; i++) {
        Output& o = g_out[i];
        if (o.kind == P_HP && (g_w[o.pin].pincaps & (1 << 2)) && (cmd(o.pin, 0xF0900) & 0x80000000u)) return true;
    }
    return false;
}

// Headphones in: speakers off. Checked twice a second from the main loop.
void jack_poll() {
    static uint64_t next;
    static int last = -1;
    uint64_t now = timer_ms();
    if (now < next || g_cmd_broken) return;
    next = now + 500;
    bool hp = hp_present();
    if ((int)hp == last) return;
    last = hp;
    for (int i = 0; i < g_nout; i++)
        if (g_out[i].kind == P_SPEAKER) set_pin(g_out[i], !hp);
    klog("hda: headphones %s", hp ? "plugged in, speakers off" : "out, speakers on");
}

bool setup_codec() {
    g_cmd_broken = false;
    uint32_t sub = param(0, 0x04);
    int start = (int)(sub >> 16) & 0xFF, n = (int)sub & 0xFF;
    g_afg = -1;
    for (int nid = start; nid < start + n; nid++)
        if ((param(nid, 0x05) & 0xFF) == 1) { g_afg = nid; break; }
    if (g_afg < 0 || g_cmd_broken) return false;
    uint32_t vendor = param(0, 0x00);
    cmd(g_afg, 0x70500);   // the function group powers up first
    delay_us(10000);
    memset(g_w, 0, sizeof(g_w));
    g_w[0].outamp = param(g_afg, 0x12);   // defaults for widgets without their own amp caps
    g_w[0].inamp = param(g_afg, 0x0D);
    sub = param(g_afg, 0x04);
    g_first = (int)(sub >> 16) & 0xFF;
    g_count = (int)sub & 0xFF;
    if (g_first <= 0 || g_first + g_count > kMaxNodes) return false;
    for (int nid = g_first; nid < g_first + g_count; nid++) {
        Widget& w = g_w[nid];
        w.caps = param(nid, 0x09);
        w.type = (uint8_t)((w.caps >> 20) & 0xF);
        if (w.caps & (1 << 3)) { w.outamp = param(nid, 0x12); w.inamp = param(nid, 0x0D); }
        if (w.type == W_PIN) { w.pincaps = param(nid, 0x0C); w.config = cmd(nid, 0xF1C00); }
        read_connections(nid);
    }
    // outputs: speakers, headphones and line outs that are wired up
    g_nout = 0;
    for (int pass = 0; pass < 2 && !g_nout; pass++) {
        for (int nid = g_first; nid < g_first + g_count && g_nout < 8; nid++) {
            Widget& w = g_w[nid];
            if (w.type != W_PIN || !(w.pincaps & (1 << 4))) continue;
            uint32_t conn = w.config >> 30, dev = (w.config >> 20) & 0xF;
            PinKind kind = dev == 1 ? P_SPEAKER : dev == 2 ? P_HP : dev == 0 ? P_LINE : P_NONE;
            if (pass == 0 && (conn == 1 || kind == P_NONE)) continue;   // nothing attached
            Output& o = g_out[g_nout];
            memset(&o, 0, sizeof(o));
            o.pin = (uint8_t)nid;
            o.kind = kind == P_NONE ? P_LINE : kind;
            if (find_path(o, nid, 0)) g_nout++;
        }
    }
    klog("hda: codec %d (%04x:%04x), %d output(s)", g_codec, vendor >> 16, vendor & 0xFFFF, g_nout);
    if (!g_nout) return false;
    for (int i = 0; i < g_nout; i++) {
        setup_output(g_out[i]);
        static const char* kinds[] = {"?", "line out", "speaker", "headphones"};
        klog("hda:   %s on pin %d, %d node(s) to converter %d", kinds[g_out[i].kind], g_out[i].pin, g_out[i].len, g_out[i].path[g_out[i].len - 1]);
    }
    return !g_cmd_broken;
}

// ---------------------------------------------------------------- stream
// Timer interrupt, every 4 ms: fill the ring up to kLeadFrames ahead of the DMA position.
void pump() {
    uint32_t pos = (r32(g_sd + SD_LPIB) / 4) % kRingFrames;
    uint32_t ahead = (g_write + kRingFrames - pos) % kRingFrames;
    if (ahead > kRingFrames / 2) {   // we fell behind (interrupts were off): start again just ahead
        g_write = (pos + 128) % kRingFrames;
        ahead = 128;
    }
    if (ahead >= kLeadFrames) return;
    uint32_t n = kLeadFrames - ahead;
    while (n) {
        uint32_t chunk = kRingFrames - g_write < n ? kRingFrames - g_write : n;
        sound_mix(g_ring + g_write * 2, chunk);
        g_write = (g_write + chunk) % kRingFrames;
        n -= chunk;
    }
}

bool start_stream() {
    uint32_t gcap = r16(GCAP);
    uint32_t iss = (gcap >> 8) & 0xF, oss = (gcap >> 12) & 0xF;
    if (!oss) { klog("hda: controller has no output streams"); return false; }
    g_sd = 0x80 + iss * 0x20;
    w8(g_sd + SD_CTL, r8(g_sd + SD_CTL) & ~2);   // stop
    w8(g_sd + SD_CTL, r8(g_sd + SD_CTL) | 1);    // reset
    wait_for([] { return r8(g_sd + SD_CTL) & 1; }, 1000);
    w8(g_sd + SD_CTL, r8(g_sd + SD_CTL) & ~1);
    wait_for([] { return !(r8(g_sd + SD_CTL) & 1); }, 1000);

    uint32_t bytes = kRingFrames * 4;
    g_ring = (int16_t*)dma_alloc(bytes);
    uint32_t* bdl = (uint32_t*)dma_alloc(16 * kBdlEntries);
    if (!g_ring || !bdl) return false;
    for (uint32_t i = 0; i < kBdlEntries; i++) {
        uint64_t a = (uint64_t)(uintptr_t)g_ring + i * (bytes / kBdlEntries);
        bdl[i * 4 + 0] = (uint32_t)a;
        bdl[i * 4 + 1] = (uint32_t)(a >> 32);
        bdl[i * 4 + 2] = bytes / kBdlEntries;
        bdl[i * 4 + 3] = 0;   // no interrupts: the timer drives us
    }
    set_base(g_sd + SD_BDPL, g_sd + SD_BDPU, bdl);
    w32(g_sd + SD_CBL, bytes);
    w16(g_sd + SD_LVI, kBdlEntries - 1);
    w16(g_sd + SD_FMT, kFormat);
    w8(g_sd + SD_CTL + 2, (uint8_t)(kStreamTag << 4));
    w8(g_sd + SD_STS, 0x1C);                     // clear status bits
    g_write = kLeadFrames;                       // the first 25 ms are silence
    w8(g_sd + SD_CTL, r8(g_sd + SD_CTL) | 2);    // run
    return true;
}

// ---------------------------------------------------------------- controller
bool reset_controller() {
    w32(INTCTL, 0);
    w8(CORBCTL, 0);
    w8(RIRBCTL, 0);
    wait_for([] { return !(r8(CORBCTL) & 2) && !(r8(RIRBCTL) & 2); }, 1000);
    w32(GCTL, r32(GCTL) & ~1u);
    if (!wait_for([] { return !(r32(GCTL) & 1); }, 100000)) return false;
    delay_us(200);
    w32(GCTL, r32(GCTL) | 1);
    if (!wait_for([] { return r32(GCTL) & 1; }, 100000)) return false;
    delay_us(2000);   // codecs announce themselves within 521 us of the reset
    return true;
}

uint32_t ring_size(uint32_t reg, uint32_t* entries) {
    uint8_t cap = r8(reg) >> 4;
    uint8_t code = (cap & 4) ? 2 : (cap & 2) ? 1 : 0;
    *entries = code == 2 ? 256 : code == 1 ? 16 : 2;
    w8(reg, (r8(reg) & ~3) | code);
    return *entries;
}

bool start_rings() {
    ring_size(CORBSIZE, &g_corb_n);
    ring_size(RIRBSIZE, &g_rirb_n);
    g_corb = (uint32_t*)dma_alloc(1024);
    g_rirb = (volatile uint64_t*)dma_alloc(2048);
    if (!g_corb || !g_rirb) return false;
    set_base(CORBLBASE, CORBUBASE, g_corb);
    w16(CORBRP, 0x8000);                                  // reset the read pointer
    wait_for([] { return r16(CORBRP) & 0x8000; }, 1000);
    w16(CORBRP, 0);
    wait_for([] { return !(r16(CORBRP) & 0x8000); }, 1000);
    w16(CORBWP, 0);
    set_base(RIRBLBASE, RIRBUBASE, (const void*)g_rirb);
    w16(RIRBWP, 0x8000);
    w16(RINTCNT, 0xFF);
    g_rirb_rp = 0;
    w8(CORBCTL, 2);
    w8(RIRBCTL, 2);
    return wait_for([] { return (r8(CORBCTL) & 2) && (r8(RIRBCTL) & 2); }, 1000);
}

bool probe(const PciDevice* d) {
    if (d->bar[0] & 1) return false;
    if ((d->bar[0] & 6) == 4 && d->bar[1]) { klog("hda: registers above 4 GiB"); return false; }
    if (g_mmio) return false;   // one sound card is enough
    pci_enable(d);
    if (d->vendor == 0x8086) {
        // Intel: traffic class 0, and make sure DMA is snooped (cache coherent)
        uint32_t v = pci_read32(d->bus, d->slot, d->func, 0x44);
        pci_write32(d->bus, d->slot, d->func, 0x44, v & ~7u);
    }
    g_mmio = (volatile uint8_t*)(uintptr_t)(d->bar[0] & ~0xFu);
    if (!reset_controller()) { klog("hda: controller did not come out of reset"); g_mmio = nullptr; return false; }
    uint16_t codecs = r16(STATESTS);
    w16(STATESTS, codecs);
    if (!start_rings()) { klog("hda: command rings did not start"); g_mmio = nullptr; return false; }
    for (int c = 0; c < 15 && g_codec < 0; c++) {
        if (!(codecs & (1 << c))) continue;
        g_codec = c;
        if (!setup_codec()) g_codec = -1;
    }
    if (g_codec < 0) { klog("hda: no codec with an audio output (codecs %04x)", codecs); g_mmio = nullptr; return false; }
    if (!start_stream()) { klog("hda: output stream did not start"); g_mmio = nullptr; return false; }
    system_add_poll_hook(jack_poll);
    sound_attach("High Definition Audio", pump);
    return true;
}

}  // namespace

extern "C" {
extern const DriverInfo klein_driver;
extern const DriverFallback klein_driver_fallback;
const DriverInfo klein_driver = {KLEIN_DRIVER_ABI, "High Definition Audio", nullptr, 0, probe};
// every HD Audio controller (class 4 multimedia, subclass 3) speaks the same register set
const DriverFallback klein_driver_fallback = {0x04, 0x03};
}
