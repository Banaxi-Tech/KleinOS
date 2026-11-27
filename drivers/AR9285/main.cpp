// Qualcomm Atheros AR9285 (PCIe 802.11n, 168C:002B).
//
// Scans 2.4 GHz channels 1-13 and joins open or WPA2-PSK (CCMP) networks, with
// encryption done in software. Transmit power is fixed at 14 dBm, using the
// card's calibration data. Everything is polled, without interrupts.
// The bring-up sequence and register values follow Linux ath9k.
#include <stddef.h>
#include <initializer_list>
#include "arch/cpu.h"
#include "arch/interrupts.h"
#include "drivers/pci.h"
#include "lib/heap.h"
#include "lib/log.h"
#include "lib/string.h"
#include "net/crypto.h"
#include "net/net.h"
#include "net/wifi.h"
#include "sys/driver.h"
#include "initvals.h"

namespace {

constexpr uint32_t AR_CR = 0x0008, AR_CR_RXE = 4, AR_CR_RXD = 0x20, AR_RXDP = 0x000C, AR_TXCFG = 0x0030, AR_RXCFG = 0x0034;
constexpr uint32_t AR_MIBC = 0x0040, AR_ISR = 0x0080, AR_RC = 0x4000, AR_RC_AHB = 1;
constexpr uint32_t AR_SREV = 0x4020, AR_SREV_VERSION2 = 0xFFFC0000, AR_SREV_TYPE2_S = 12, AR_SREV_REVISION2 = 0xF00;
constexpr uint32_t AR_SREV_VERSION_9285 = 0xC0, AR_SREV_REVISION_9285_12 = 2;
constexpr uint32_t AR_AHB_MODE = 0x4024, AR_AHB_PREFETCH_RD_EN = 4, AR_GPIO_INPUT_EN_VAL = 0x4054, AR_GPIO_JTAG_DISABLE = 0x20000;
constexpr uint32_t AR_EEPROM_STATUS_DATA = 0x407C, AR_EEPROM_BUSY = 0x10000, AR_EEPROM_PROT = 0x40000;
constexpr uint32_t AR_RTC_RC = 0x7000, AR_RTC_RC_M = 3, AR_RTC_RC_MAC_WARM = 1, AR_RTC_RC_MAC_COLD = 2;
constexpr uint32_t AR_RTC_PLL_CONTROL = 0x7014, AR_RTC_RESET = 0x7040, AR_RTC_STATUS = 0x7044, AR_RTC_STATUS_M = 0xF, AR_RTC_STATUS_ON = 2;
constexpr uint32_t AR_RTC_SLEEP_CLK = 0x7048, AR_RTC_FORCE_DERIVED_CLK = 2, AR_RTC_FORCE_WAKE = 0x704C, AR_RTC_FORCE_WAKE_BOTH = 3;
constexpr uint32_t AR_STA_ID0 = 0x8000, AR_STA_ID1 = 0x8004, AR_STA_ID1_RTS_USE_DEF = 0x00800000, AR_STA_ID1_KSRCH_MODE = 0x10000000;
constexpr uint32_t AR_BSS_ID0 = 0x8008, AR_BSS_ID1 = 0x800C, AR_RSSI_THR = 0x8018, INIT_RSSI_THR = 0x700;
constexpr uint32_t AR_RX_FILTER = 0x803C, RX_UCAST = 1, RX_MCAST = 2, RX_BCAST = 4, RX_BEACON = 0x10;
constexpr uint32_t AR_MCAST_FIL0 = 0x8040, AR_MCAST_FIL1 = 0x8044, AR_DIAG_SW = 0x8048, AR_DIAG_RX_DIS = 0x20, AR_DIAG_RX_ABORT = 0x02000000;
constexpr uint32_t AR_DEF_ANTENNA = 0x8058, AR_BSSMSKL = 0x80E0, AR_BSSMSKU = 0x80E4, AR_RXFIFO_CFG = 0x8114, AR_PHY_ERR = 0x810C, AR_2040_MODE = 0x8318;
constexpr uint32_t AR_PHY_BASE = 0x9800, AR_PHY_TURBO = 0x9804, AR_PHY_FC_ENABLE_DAC_FIFO = 0x800, AR_PHY_FC_DYN2040_EN = 4;
constexpr uint32_t AR_PHY_FC_HT_EN = 0x40, AR_PHY_FC_SHORT_GI_40 = 0x80, AR_PHY_FC_WALSH = 0x100, AR_PHY_FC_SINGLE_HT_LTF1 = 0x200;
constexpr uint32_t AR_PHY_TIMING3 = 0x9814, AR_PHY_ACTIVE = 0x981C, AR_PHY_ADC_CTL = 0x982C, AR_PHY_ADC_CTL_OFF_PWDADC = 0x8000;
constexpr uint32_t AR_PHY_SETTLING = 0x9844, AR_PHY_RXGAIN = 0x9848, AR_PHY_DESIRED_SZ = 0x9850, AR_PHY_AGC_CONTROL = 0x9860, AR_PHY_CCA = 0x9864;
constexpr uint32_t AGC_CAL = 1, AGC_NF = 2, AGC_ENABLE_NF = 0x8000, AGC_FLTR_CAL = 0x10000, AGC_NO_UPDATE_NF = 0x20000;
constexpr uint32_t AR_PHY_SYNTH_CONTROL = 0x9874, AR_PHY_RX_DELAY = 0x9914, AR_PHY_TIMING_CTRL4 = 0x9920;
constexpr uint32_t AR_PHY_SWITCH_CHAIN_0 = 0x9960, AR_PHY_SWITCH_COM = 0x9964, AR_PHY_RX_CHAINMASK = 0x99A4, AR_PHY_MULTICHAIN_GAIN_CTL = 0x99AC, AR_PHY_EXT_CCA0 = 0x99B8;
constexpr uint32_t AR_PHY_MODE = 0xA200, AR_PHY_MODE_DYNAMIC = 4, AR_PHY_CCK_DETECT = 0xA208, AR_PHY_GAIN_2GHZ = 0xA20C, AR_PHY_TPCRG1 = 0xA258, TPCRG1_PD_CAL = 0x400000;
constexpr uint32_t AR_PHY_CL_CAL_CTL = 0xA358, CL_CAL_ENABLE = 2, PARALLEL_CAL_ENABLE = 1, AR_PHY_CAL_CHAINMASK = 0xA39C;
constexpr uint32_t kEepromBase = 0x2000, kEepromMagic = 0xA55A;
constexpr int kEep4kStart = 64, kEepWords = 96, kModalOffset = 52, kTimeoutUs = 100000;

constexpr uint32_t AR_QTXDP0 = 0x0800, AR_Q_TXE = 0x0840, AR_QMISC0 = 0x09C0, AR_Q_MISC_DCU_EARLY_TERM_REQ = 0x800;
constexpr uint32_t AR_DQCUMASK0 = 0x1000, AR_DLCL_IFS0 = 0x1040, AR_DRETRY_LIMIT0 = 0x1080, AR_DCHNTIME0 = 0x10C0, AR_DMISC0 = 0x1100;
constexpr uint32_t AR_D_MISC_CW_BKOFF_EN = 0x1000, AR_D_MISC_FRAG_WAIT_EN = 0x100;
constexpr uint32_t AR_PHY_POWER_TX_RATE1 = 0x9934, AR_PHY_POWER_TX_RATE2 = 0x9938, AR_PHY_POWER_TX_RATE_MAX = 0x993C, AR_PHY_POWER_TX_RATE3 = 0xA234;
constexpr uint32_t AR_PHY_POWER_TX_RATE4 = 0xA238, AR_PHY_POWER_TX_RATE5 = 0xA38C, AR_PHY_POWER_TX_RATE6 = 0xA390, AR_PHY_TPCRG5 = 0xA26C;
constexpr uint32_t AN_RF2G1 = 0x7820, AN_RF2G2 = 0x7824, AN_RF2G3 = 0x7828, AN_RF2G4 = 0x782C, AN_RF2G6 = 0x7834, AN_RF2G7 = 0x7838, AN_RF2G8 = 0x783C;
constexpr uint32_t AN_RF2G9 = 0x7840, AN_RXTXBB1 = 0x7854, AN_TOP2 = 0x7868, AN_TOP3 = 0x786C, AR_AN_SYNTH9 = 0x7868, RF2G3_PDVCCOMP = 0x02000000;
constexpr int kTxPowerHalfDbm = 28, kPwrTableOffset = -5, kCalPierOffset = 120, kCalDataOffset = 123, kIcepts = 5, kNumPdadc = 128;
constexpr uint32_t kTxPadForFcs = 4, kNoAck = 0x01000000, kClrDestMask = 0x01000000;
constexpr uint32_t kRateMgmt = 0x1B, kChainSelAll = (1u << 2) | (1u << 7) | (1u << 12) | (1u << 17);
constexpr uint32_t kDataRates = 0x09 | (0x0A << 8) | (0x0B << 16) | (0x1Bu << 24), kDataTries = (2u << 16) | (2u << 20) | (2u << 24) | (4u << 28);
constexpr uint16_t kDuration = 314;
constexpr int kTxSlots = 24, kMaxFrame = 1600;
constexpr uint64_t kRetryMs = 400, kHandshakeMs = 4000, kBeaconLossMs = 10000, kTxStuckMs = 300;
constexpr int kMaxTries = 6;
constexpr uint8_t kBroadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
constexpr uint8_t kRsnIe[22] = {48, 20, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 4, 1, 0, 0, 0x0F, 0xAC, 2, 0, 0};
constexpr uint16_t KI_V2 = 2, KI_PAIRWISE = 8, KI_INSTALL = 0x40, KI_ACK = 0x80, KI_MIC = 0x100, KI_SECURE = 0x200, KI_ENCRYPTED = 0x1000;

constexpr int kRxDescs = 48, kRxBufBytes = 2400, kMaxSeen = 32;
constexpr uint64_t kDwellMs = 130;
constexpr uint32_t RX_DONE = 1, RX_FRAME_OK = 2, RX_DECRYPT_ERR = 8;
constexpr uint32_t AR_KEYTABLE_0 = 0x8800, kKeyEntries = 128, kKeyTypeClear = 7;

struct RxDesc {
    volatile uint32_t link, data, ctl0, ctl1;
    volatile uint32_t status[9];
    uint32_t pad[3];
};

volatile uint8_t* g_mmio;
uint8_t g_mac[6];
uint8_t g_eep[kEepWords * 2];
RxDesc* g_desc[kRxDescs];
uint8_t* g_buf[kRxDescs];
int g_rx_next;

bool g_scanning;
int g_channel;
uint64_t g_dwell_end;
uint32_t g_frames;
net::WifiNetwork g_seen[kMaxSeen];
int8_t g_rssi[kMaxSeen];
struct SeenInfo { uint8_t bssid[6]; uint8_t channel; bool rsn_ok; };
SeenInfo g_info[kMaxSeen];
int g_seen_count;
net::WifiStatus g_status = net::WifiStatus::Idle;
void (*g_deliver)(const uint8_t*, size_t);

enum class Join { None, Auth, Assoc, WaitM1, WaitM3, Up };
Join g_join = Join::None;
char g_ssid[33];
uint8_t g_bssid[6];
int g_join_channel, g_tries;
uint16_t g_aid, g_seq;
bool g_secured;
uint64_t g_state_time, g_last_beacon;
uint8_t g_pmk[32], g_kck[16], g_kek[16], g_snonce[32], g_anonce[32];
crypto::Aes128 g_tk, g_gtk[4];
bool g_tk_set, g_gtk_set[4];
uint64_t g_tx_pn, g_rx_pn;

struct TxSlot { uint16_t len; bool basic_rate; uint8_t data[kMaxFrame]; };
TxSlot* g_tx;
volatile uint32_t* g_tx_desc;
int g_tx_head, g_tx_tail;
bool g_tx_busy;
uint64_t g_tx_time;
uint32_t g_tx_fail, g_tx_ok, g_rx_hw_decrypt_err, g_rx_data, g_rx_no_key, g_rx_replay, g_rx_bad_mic, g_rx_delivered, g_rx_resync;
uint64_t g_stats_time;
int g_stats_left;
uint8_t g_eth[kMaxFrame];
net::NicDriver g_nic;

uint32_t rd(uint32_t r) { return *(volatile uint32_t*)(g_mmio + r); }
void wr(uint32_t r, uint32_t v) { *(volatile uint32_t*)(g_mmio + r) = v; }
void set_bits(uint32_t r, uint32_t b) { wr(r, rd(r) | b); }
void clr_bits(uint32_t r, uint32_t b) { wr(r, rd(r) & ~b); }
void field(uint32_t r, uint32_t mask, int shift, uint32_t v) { wr(r, (rd(r) & ~mask) | ((v << shift) & mask)); }
void delay_us(int us) { for (int i = 0; i < us; i++) io_wait(); }   // port 0x80 takes about 1us

bool wait_reg(uint32_t r, uint32_t mask, uint32_t want) {
    for (int t = 0; t < kTimeoutUs; t += 10) {
        if ((rd(r) & mask) == want) return true;
        delay_us(10);
    }
    return false;
}

bool mac_reset(uint32_t kind) {
    wr(AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_BOTH);
    wr(AR_RC, AR_RC_AHB);
    wr(AR_RTC_RC, kind);
    delay_us(50);
    wr(AR_RTC_RC, 0);
    if (!wait_reg(AR_RTC_RC, AR_RTC_RC_M, 0)) return false;
    wr(AR_RC, 0);
    wr(AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_BOTH);
    return wait_reg(AR_RTC_STATUS, AR_RTC_STATUS_M, AR_RTC_STATUS_ON);
}

bool power_on_reset() {
    wr(AR_RTC_FORCE_WAKE, AR_RTC_FORCE_WAKE_BOTH);
    wr(AR_RC, AR_RC_AHB);
    wr(AR_RTC_RESET, 0);
    delay_us(2);
    wr(AR_RC, 0);
    wr(AR_RTC_RESET, 1);
    if (!wait_reg(AR_RTC_STATUS, AR_RTC_STATUS_M, AR_RTC_STATUS_ON)) return false;
    return mac_reset(AR_RTC_RC_MAC_WARM);
}

bool eeprom_read(int word, uint16_t* out) {
    (void)rd(kEepromBase + (word << 2));   // the read latches the address
    if (!wait_reg(AR_EEPROM_STATUS_DATA, AR_EEPROM_BUSY | AR_EEPROM_PROT, 0)) return false;
    *out = rd(AR_EEPROM_STATUS_DATA) & 0xFFFF;
    return true;
}

uint32_t eep32(int off) { return g_eep[off] | (g_eep[off + 1] << 8) | (g_eep[off + 2] << 16) | ((uint32_t)g_eep[off + 3] << 24); }
uint8_t modal(int off) { return g_eep[kModalOffset + off]; }
uint32_t field_of(uint32_t r, uint32_t mask, int shift) { return (rd(r) & mask) >> shift; }
void analog(uint32_t r, uint32_t mask, int shift, uint32_t v) { field(r, mask, shift, v); delay_us(100); }

template <size_t N>
void write_table(const InitReg (&t)[N]) {
    for (const InitReg& r : t) {
        wr(r.reg, r.val);
        if (r.reg >= 0x7800 && r.reg < 0x78A0) delay_us(100);   // analog shift registers
    }
}

// Receive-side board settings from the card's own EEPROM.
void set_board_values() {
    uint32_t minor = (g_eep[4] | (g_eep[5] << 8)) & 0xFFF;
    wr(AR_PHY_SWITCH_COM, eep32(kModalOffset + 4));
    wr(AR_PHY_SWITCH_CHAIN_0, eep32(kModalOffset));
    uint32_t iq = rd(AR_PHY_TIMING_CTRL4) & ~0x7FFu;
    wr(AR_PHY_TIMING_CTRL4, iq | ((uint32_t)(modal(22) & 0x3F) << 5) | (modal(23) & 0x1F));
    uint32_t atten = 23;
    if (minor >= 3) {
        atten = modal(10);
        for (uint32_t r : {AR_PHY_GAIN_2GHZ, AR_PHY_GAIN_2GHZ + 0x1000}) {
            field(r, 0x0001F000, 12, modal(32));
            field(r, 0x0000003F, 0, modal(31));
            field(r, 0x003E0000, 17, modal(35));
            field(r, 0x00000FC0, 6, modal(34));
        }
    }
    for (uint32_t r : {AR_PHY_RXGAIN, AR_PHY_RXGAIN + 0x1000}) {
        field(r, 0x00003F80, 7, atten);
        field(r, 0x001FC000, 14, modal(11));
    }
    if (modal(37) >= 3) {
        uint32_t c1 = modal(39) >> 4, c2 = modal(41) >> 4;
        uint32_t v = rd(AR_PHY_MULTICHAIN_GAIN_CTL) & ~0x7F000000u;
        v |= ((c1 & 1) << 24) | ((c2 & 3) << 25) | (((c2 >> 2) & 3) << 27) | (((c1 >> 1) & 1) << 29) | (((c1 >> 2) & 1) << 30);
        wr(AR_PHY_MULTICHAIN_GAIN_CTL, v);
        field(AR_PHY_CCK_DETECT, 0x2000, 13, (c1 >> 3) & 1);
    }
    field(AR_PHY_SETTLING, 0x00003F80, 7, modal(9));
    field(AR_PHY_DESIRED_SZ, 0xFF, 0, modal(12));
    field(AR_PHY_CCA, 0x000FF000, 12, modal(18));
    field(AR_PHY_EXT_CCA0, 0xFF, 0, modal(18));
}

// Output-stage drive levels for the five power ranges, from EEPROM.
void set_tx_board_values() {
    uint8_t ob[5], db1[5], db2[5];
    uint8_t version = modal(37);
    uint8_t ob0 = modal(25) & 15, ob1 = modal(25) >> 4, d0 = modal(26) & 15, d1 = modal(26) >> 4, e0 = modal(36) & 15, e1 = modal(36) >> 4;
    if (version >= 2) {
        ob[0] = ob0; ob[1] = ob1; ob[2] = modal(38) & 15; ob[3] = modal(38) >> 4; ob[4] = modal(39) & 15;
        db1[0] = d0; db1[1] = d1; db1[2] = modal(40) & 15; db1[3] = modal(40) >> 4; db1[4] = modal(41) & 15;
        db2[0] = e0; db2[1] = e1; db2[2] = modal(42) & 15; db2[3] = modal(42) >> 4; db2[4] = modal(43) & 15;
    } else {
        ob[0] = ob0; db1[0] = d0; db2[0] = version == 1 ? e0 : d0;
        for (int i = 1; i < 5; i++) { ob[i] = version == 1 ? ob1 : ob0; db1[i] = version == 1 ? d1 : d0; db2[i] = version == 1 ? e1 : d0; }
    }
    static const struct { uint32_t reg, mask; int shift; } f[15] = {
        {AN_RF2G3, 0x00E00000, 21}, {AN_RF2G3, 0x001C0000, 18}, {AN_RF2G3, 0x00038000, 15}, {AN_RF2G3, 0x00007000, 12}, {AN_RF2G3, 0x00000E00, 9},
        {AN_RF2G3, 0x000001C0, 6},  {AN_RF2G3, 0x00000038, 3},  {AN_RF2G3, 0x00000007, 0},  {AN_RF2G4, 0xE0000000, 29}, {AN_RF2G4, 0x1C000000, 26},
        {AN_RF2G4, 0x03800000, 23}, {AN_RF2G4, 0x00700000, 20}, {AN_RF2G4, 0x000E0000, 17}, {AN_RF2G4, 0x0001C000, 14}, {AN_RF2G4, 0x00003800, 11},
    };
    for (int i = 0; i < 5; i++) {
        analog(f[i].reg, f[i].mask, f[i].shift, ob[i]);
        analog(f[5 + i].reg, f[5 + i].mask, f[5 + i].shift, db1[i]);
        analog(f[10 + i].reg, f[10 + i].mask, f[10 + i].shift, db2[i]);
    }
}

bool lower_upper(uint8_t target, const uint8_t* list, int n, int* l, int* r) {
    if (target <= list[0]) { *l = *r = 0; return true; }
    if (target >= list[n - 1]) { *l = *r = n - 1; return true; }
    for (int i = 0; i < n - 1; i++) {
        if (list[i] == target) { *l = *r = i; return true; }
        if (target < list[i + 1]) { *l = i; *r = i + 1; return false; }
    }
    *l = *r = n - 1;
    return true;
}

void fill_vpd(uint8_t pmin, uint8_t pmax, const uint8_t* pwr, const uint8_t* vpd, uint8_t* out) {
    uint8_t cur = pmin;
    for (int i = 0; i <= (pmax - pmin) / 2 && i < 64; i++) {
        int l, r;
        lower_upper(cur, pwr, kIcepts, &l, &r);
        if (r < 1) r = 1;
        if (l == kIcepts - 1) l = kIcepts - 2;
        out[i] = pwr[l] == pwr[r] ? vpd[l] : (uint8_t)(((cur - pwr[l]) * vpd[r] + (pwr[r] - cur) * vpd[l]) / (pwr[r] - pwr[l]));
        cur += 2;
    }
}

// Closed-loop power control table, interpolated from the card's factory calibration.
void set_power_cal(uint32_t freq) {
    static uint8_t table_l[2][64], table_r[2][64], table_i[2][64], pdadc[kNumPdadc];
    uint32_t mask = modal(20);
    uint32_t minor = (g_eep[4] | (g_eep[5] << 8)) & 0xFFF;
    uint32_t overlap = minor >= 2 ? modal(24) : field_of(AR_PHY_TPCRG5, 0xF, 0);
    int gains = 0;
    uint16_t gain_values[2] = {0, 0};
    for (int i = 1; i <= 4 && gains < 2; i++)
        if ((mask >> (4 - i)) & 1) gain_values[gains++] = (uint16_t)(4 - i);
    if (!gains) return;
    field(AR_PHY_TPCRG1, 0x0000C000, 14, (uint32_t)(gains - 1) & 3);
    field(AR_PHY_TPCRG1, 0x00030000, 16, gain_values[0]);
    field(AR_PHY_TPCRG1, 0x000C0000, 18, gain_values[1]);
    field(AR_PHY_TPCRG1, 0x00300000, 20, 0);

    const uint8_t* piers = g_eep + kCalPierOffset;
    int num = 0;
    while (num < 3 && piers[num] != 0xFF) num++;
    if (!num) return;
    uint8_t fbin = (uint8_t)(freq - 2300);
    int il, ir;
    bool match = lower_upper(fbin, piers, num, &il, &ir);
    const uint8_t* cal_l = g_eep + kCalDataOffset + il * 20;
    const uint8_t* cal_r = g_eep + kCalDataOffset + ir * 20;
    uint8_t minp[2] = {0, 0}, maxp[2] = {0, 0};
    for (int i = 0; i < gains; i++) {
        const uint8_t* pl = cal_l + i * kIcepts, *vl = cal_l + 10 + i * kIcepts;
        const uint8_t* pr = cal_r + i * kIcepts, *vr = cal_r + 10 + i * kIcepts;
        if (match) {
            minp[i] = pl[0];
            maxp[i] = pl[kIcepts - 1];
            fill_vpd(minp[i], maxp[i], pl, vl, table_i[i]);
        } else {
            minp[i] = pl[0] > pr[0] ? pl[0] : pr[0];
            maxp[i] = pl[kIcepts - 1] < pr[kIcepts - 1] ? pl[kIcepts - 1] : pr[kIcepts - 1];
            fill_vpd(minp[i], maxp[i], pl, vl, table_l[i]);
            fill_vpd(minp[i], maxp[i], pr, vr, table_r[i]);
            int span = piers[ir] - piers[il];
            for (int j = 0; j <= (maxp[i] - minp[i]) / 2 && j < 64; j++)
                table_i[i][j] = (uint8_t)(((fbin - piers[il]) * table_r[i][j] + (piers[ir] - fbin) * table_l[i][j]) / span);
        }
    }
    uint16_t bounds[4];
    int k = 0, i;
    for (i = 0; i < gains; i++) {
        bounds[i] = i == gains - 1 ? (uint16_t)(maxp[i] / 2) : (uint16_t)((maxp[i] + minp[i + 1]) / 4);
        if (bounds[i] > 63) bounds[i] = 63;
        int ss = i == 0 ? 0 - (minp[i] / 2) : (bounds[i - 1] - (minp[i] / 2)) - (int)overlap + 1;
        int step = table_i[i][1] - table_i[i][0];
        if (step < 1) step = 1;
        while (ss < 0 && k < kNumPdadc - 1) {
            int v = table_i[i][0] + ss * step;
            pdadc[k++] = (uint8_t)(v < 0 ? 0 : v);
            ss++;
        }
        int size = (uint8_t)((maxp[i] - minp[i]) / 2 + 1);
        int target = (uint8_t)(bounds[i] + overlap - (minp[i] / 2));
        int maxi = target < size ? target : size;
        while (ss < maxi && k < kNumPdadc - 1) pdadc[k++] = table_i[i][ss++];
        step = table_i[i][size - 1] - table_i[i][size - 2];
        if (step < 1) step = 1;
        if (target >= maxi) {
            while (ss <= target && k < kNumPdadc - 1) {
                int v = table_i[i][size - 1] + (ss - maxi + 1) * step;
                pdadc[k++] = (uint8_t)(v > 255 ? 255 : v);
                ss++;
            }
        }
    }
    for (; i < 4; i++) bounds[i] = 58;
    while (k < kNumPdadc) { pdadc[k] = pdadc[k - 1]; k++; }
    wr(AR_PHY_TPCRG5, (overlap & 0xF) | ((uint32_t)bounds[0] << 4) | ((uint32_t)bounds[1] << 10) | ((uint32_t)bounds[2] << 16) | ((uint32_t)bounds[3] << 22));
    for (int j = 0; j < 32; j++)
        wr(AR_PHY_BASE + (672 << 2) + (uint32_t)j * 4, pdadc[j * 4] | (pdadc[j * 4 + 1] << 8) | (pdadc[j * 4 + 2] << 16) | ((uint32_t)pdadc[j * 4 + 3] << 24));
}

void set_tx_power(uint32_t freq) {
    set_power_cal(freq);
    uint32_t v = (uint32_t)(kTxPowerHalfDbm - kPwrTableOffset * 2) & 0x3F;
    uint32_t all = v | (v << 8) | (v << 16) | (v << 24);
    for (uint32_t r : {AR_PHY_POWER_TX_RATE1, AR_PHY_POWER_TX_RATE2, AR_PHY_POWER_TX_RATE3, AR_PHY_POWER_TX_RATE4, AR_PHY_POWER_TX_RATE5, AR_PHY_POWER_TX_RATE6}) wr(r, all);
    wr(AR_PHY_POWER_TX_RATE_MAX, 63);
}

void pa_calibrate() {
    if (g_eep[31] == 1) return;           // high power gain tables don't use it
    static const uint32_t regs[7] = {0x786C, 0x7854, 0x7820, 0x7824, 0x7868, 0x783C, 0x7838};
    uint32_t saved[7];
    for (int i = 0; i < 7; i++) saved[i] = rd(regs[i]);
    clr_bits(AN_RF2G6, 1);
    set_bits(0x9808, 1u << 27);
    field(AN_TOP3, 0x00800000, 23, 1);
    field(AN_RXTXBB1, 0x20, 5, 1);
    field(AN_RXTXBB1, 0x80, 7, 1);
    field(AN_RXTXBB1, 0x100, 8, 1);
    field(AN_RF2G2, 0x1000, 12, 0);
    field(AN_RF2G7, 2, 1, 0);
    field(AN_RF2G1, 0x800, 11, 0);
    field(AN_RF2G1, 0x02000000, 25, 0);
    field(AN_RF2G1, 0x01000000, 24, 0);
    field(AN_RF2G1, 0x00800000, 23, 0);
    field(AN_RF2G8, 0x0001C000, 14, 7);
    field(AN_RF2G7, 0xE0000000, 29, 0);
    uint32_t ccomp = field_of(AN_RF2G6, 0x7800, 11);
    field(AN_RF2G6, 0x7800, 11, 0xF);
    wr(AN_TOP2, 0xCA0358A0);
    delay_us(30);
    field(AN_RF2G6, 0x03F00000, 20, 0);
    field(AN_RF2G3, RF2G3_PDVCCOMP, 25, 0);
    for (int i = 6; i > 0; i--) {
        set_bits(AN_RF2G6, 1u << (19 + i));
        delay_us(1);
        uint32_t v = rd(AN_RF2G6) & ~(1u << (19 + i));
        v |= (rd(AN_RF2G9) & 1) << (19 + i);
        wr(AN_RF2G6, v);
    }
    field(AN_RF2G3, RF2G3_PDVCCOMP, 25, 1);
    delay_us(1);
    field(AN_RF2G3, RF2G3_PDVCCOMP, 25, rd(AN_RF2G9) & 1);
    set_bits(AN_RF2G6, 1);
    clr_bits(0x9808, 1u << 27);
    for (int i = 0; i < 7; i++) wr(regs[i], saved[i]);
    field(AN_RF2G6, 0x7800, 11, ccomp);
}

void init_tx_queue() {
    wr(AR_DQCUMASK0, 1);
    wr(AR_DLCL_IFS0, 15 | (1023u << 10) | (2u << 20));
    wr(AR_DRETRY_LIMIT0, (32u << 8) | (32u << 14) | 10);
    wr(AR_QMISC0, AR_Q_MISC_DCU_EARLY_TERM_REQ);
    wr(AR_DMISC0, AR_D_MISC_CW_BKOFF_EN | AR_D_MISC_FRAG_WAIT_EN | 2);
    wr(AR_DCHNTIME0, 0);
    g_tx_busy = false;
}

void set_delta_slope(uint32_t freq) {
    uint32_t coef = 0x64000000u / freq;
    uint32_t e = 31;
    while (e > 0 && !((coef >> e) & 1)) e--;
    e = 14 - (e - 24);
    uint32_t man = (coef + (1u << (24 - e - 1))) >> (24 - e);
    field(AR_PHY_TIMING3, 0xFFFE0000, 17, man);
    field(AR_PHY_TIMING3, 0x0001E000, 13, e - 16);
}

bool calibrate() {
    set_bits(AR_PHY_CL_CAL_CTL, CL_CAL_ENABLE | PARALLEL_CAL_ENABLE);
    set_bits(AR_PHY_TURBO, AR_PHY_FC_DYN2040_EN);
    clr_bits(AR_PHY_AGC_CONTROL, AGC_FLTR_CAL);
    clr_bits(AR_PHY_TPCRG1, TPCRG1_PD_CAL);
    set_bits(AR_PHY_AGC_CONTROL, AGC_CAL);
    if (!wait_reg(AR_PHY_AGC_CONTROL, AGC_CAL, 0)) return false;
    clr_bits(AR_PHY_TURBO, AR_PHY_FC_DYN2040_EN);
    clr_bits(AR_PHY_CL_CAL_CTL, PARALLEL_CAL_ENABLE | CL_CAL_ENABLE);

    clr_bits(AR_PHY_ADC_CTL, AR_PHY_ADC_CTL_OFF_PWDADC);
    set_bits(AR_PHY_AGC_CONTROL, AGC_FLTR_CAL);
    set_bits(AR_PHY_TPCRG1, TPCRG1_PD_CAL);
    set_bits(AR_PHY_AGC_CONTROL, AGC_CAL);
    if (!wait_reg(AR_PHY_AGC_CONTROL, AGC_CAL, 0)) return false;
    set_bits(AR_PHY_ADC_CTL, AR_PHY_ADC_CTL_OFF_PWDADC);
    clr_bits(AR_PHY_CL_CAL_CTL, CL_CAL_ENABLE);
    clr_bits(AR_PHY_AGC_CONTROL, AGC_FLTR_CAL);

    pa_calibrate();
    clr_bits(AR_PHY_AGC_CONTROL, AGC_NO_UPDATE_NF);
    set_bits(AR_PHY_AGC_CONTROL, AGC_ENABLE_NF);
    set_bits(AR_PHY_AGC_CONTROL, AGC_NF);
    return true;
}

void arm_descriptor(int i) {
    RxDesc* d = g_desc[i];
    d->ctl0 = 0;
    d->ctl1 = kRxBufBytes & 0xFFF;
    for (int s = 0; s < 9; s++) d->status[s] = 0;
}

void start_rx() {
    for (int i = 0; i < kRxDescs; i++) arm_descriptor(i);
    g_rx_next = 0;
    __sync_synchronize();
    wr(AR_RXDP, (uint32_t)(uintptr_t)g_desc[0]);
    wr(AR_CR, AR_CR_RXE);
    wr(AR_RX_FILTER, RX_UCAST | RX_BCAST | RX_BEACON | (g_join != Join::None ? RX_MCAST : 0));
    wr(AR_PHY_ERR, 0);
    wr(AR_MCAST_FIL0, 0xFFFFFFFF);
    wr(AR_MCAST_FIL1, 0xFFFFFFFF);
    wr(AR_MIBC, 0);
    clr_bits(AR_DIAG_SW, AR_DIAG_RX_DIS | AR_DIAG_RX_ABORT);
}

void stop_rx() {
    set_bits(AR_DIAG_SW, AR_DIAG_RX_DIS | AR_DIAG_RX_ABORT);
    wr(AR_CR, AR_CR_RXD);
    for (int t = 0; t < 10000 && (rd(AR_CR) & AR_CR_RXE); t += 10) delay_us(10);
}

// Encryption is done in software: every key slot must be empty, or the chip tries to decrypt with whatever it holds.
void clear_key_cache() {
    for (uint32_t i = 0; i < kKeyEntries; i++) {
        uint32_t base = AR_KEYTABLE_0 + i * 32;
        for (uint32_t r = 0; r < 20; r += 4) wr(base + r, 0);
        wr(base + 20, kKeyTypeClear);
        wr(base + 24, 0);
        wr(base + 28, 0);
    }
}

// Full reset onto one 2.4 GHz channel; matches the order of ath9k_hw_reset.
bool tune(uint32_t freq) {
    stop_rx();
    wr(AR_PHY_ACTIVE, 0);
    delay_us(100);
    if (!mac_reset(AR_RTC_RC_MAC_WARM | AR_RTC_RC_MAC_COLD)) return false;
    wr(AR_RTC_PLL_CONTROL, (5u << 10) | 0x2C);
    delay_us(100);
    wr(AR_RTC_SLEEP_CLK, AR_RTC_FORCE_DERIVED_CLK);
    set_bits(AR_GPIO_INPUT_EN_VAL, AR_GPIO_JTAG_DISABLE);

    wr(AR_PHY_BASE, 7);
    write_table(kModes);
    bool high = g_eep[31] == 1, xe = (rd(AR_AN_SYNTH9) & 7) == 1;
    if (high && xe) write_table(kTxGainXeHigh);
    else if (high) write_table(kTxGainHigh);
    else if (xe) write_table(kTxGainXeNormal);
    else write_table(kTxGainOriginal);
    write_table(kCommon);
    wr(AR_PHY_TURBO, AR_PHY_FC_HT_EN | AR_PHY_FC_SHORT_GI_40 | AR_PHY_FC_SINGLE_HT_LTF1 | AR_PHY_FC_WALSH |
                         (rd(AR_PHY_TURBO) & AR_PHY_FC_ENABLE_DAC_FIFO));
    wr(AR_2040_MODE, 0);
    wr(AR_PHY_RX_CHAINMASK, 1);
    wr(AR_PHY_CAL_CHAINMASK, 1);
    set_tx_power(freq);
    wr(AR_PHY_MODE, AR_PHY_MODE_DYNAMIC);
    set_delta_slope(freq);
    set_board_values();
    set_tx_board_values();

    wr(AR_STA_ID0, g_mac[0] | (g_mac[1] << 8) | (g_mac[2] << 16) | ((uint32_t)g_mac[3] << 24));
    wr(AR_STA_ID1, g_mac[4] | (g_mac[5] << 8) | AR_STA_ID1_RTS_USE_DEF | AR_STA_ID1_KSRCH_MODE);
    wr(AR_BSSMSKL, 0xFFFFFFFF);
    wr(AR_BSSMSKU, 0xFFFF);
    wr(AR_BSS_ID0, g_bssid[0] | (g_bssid[1] << 8) | (g_bssid[2] << 16) | ((uint32_t)g_bssid[3] << 24));
    wr(AR_BSS_ID1, g_bssid[4] | (g_bssid[5] << 8) | ((uint32_t)g_aid << 16));
    wr(AR_DEF_ANTENNA, 1);
    wr(AR_ISR, 0xFFFFFFFF);
    wr(AR_RSSI_THR, INIT_RSSI_THR);

    uint32_t synth = rd(AR_PHY_SYNTH_CONTROL) & 0xC0000000;
    wr(AR_PHY_SYNTH_CONTROL, synth | (1u << 29) | (1u << 28) | ((freq * 0x10000) / 15));

    set_bits(AR_AHB_MODE, AR_AHB_PREFETCH_RD_EN);
    wr(AR_TXCFG, (rd(AR_TXCFG) & ~7u) | 5);
    wr(AR_RXCFG, (rd(AR_RXCFG) & ~7u) | 5);
    wr(AR_RXFIFO_CFG, 0x200);

    uint32_t delay = (rd(AR_PHY_RX_DELAY) & 0x3FFF) / 10 + 100;
    wr(AR_PHY_ACTIVE, 1);
    delay_us((int)delay);
    if (!calibrate()) klog("ar9285: calibration timed out at %u MHz", freq);
    init_tx_queue();
    if (g_join != Join::None) clear_key_cache();
    start_rx();
    return true;
}

void remember(const char* ssid, int len, bool secured, int rssi, const uint8_t* bssid, int channel, bool rsn_ok) {
    if (len <= 0 || len > 32 || !ssid[0]) return;      // hidden network
    for (int i = 0; i < g_seen_count; i++) {
        if (strlen(g_seen[i].ssid) != (size_t)len || memcmp(g_seen[i].ssid, ssid, (size_t)len)) continue;
        if (rssi > g_rssi[i]) {
            g_rssi[i] = (int8_t)rssi;
            memcpy(g_info[i].bssid, bssid, 6);
            g_info[i].channel = (uint8_t)channel;
            g_info[i].rsn_ok = rsn_ok;
        }
        return;
    }
    if (g_seen_count == kMaxSeen) return;
    net::WifiNetwork& n = g_seen[g_seen_count];
    memcpy(n.ssid, ssid, (size_t)len);
    n.ssid[len] = 0;
    n.secured = secured;
    memcpy(g_info[g_seen_count].bssid, bssid, 6);
    g_info[g_seen_count].channel = (uint8_t)channel;
    g_info[g_seen_count].rsn_ok = rsn_ok;
    g_rssi[g_seen_count++] = (int8_t)rssi;
}

// WPA2-PSK with CCMP for both unicast and group traffic is what we can join.
bool rsn_usable(const uint8_t* ie, uint32_t n) {
    if (n < 18 || memcmp(ie + 2, "\x00\x0F\xAC\x04", 4)) return false;
    uint32_t pairs = ie[6] | (ie[7] << 8), p = 8;
    bool ccmp = false;
    for (uint32_t i = 0; i < pairs && p + 4 <= n; i++, p += 4)
        if (!memcmp(ie + p, "\x00\x0F\xAC\x04", 4)) ccmp = true;
    if (!ccmp || p + 2 > n) return false;
    uint32_t akms = ie[p] | (ie[p + 1] << 8);
    p += 2;
    for (uint32_t i = 0; i < akms && p + 4 <= n; i++, p += 4)
        if (!memcmp(ie + p, "\x00\x0F\xAC\x02", 4)) return true;
    return false;
}

void parse_beacon(const uint8_t* f, uint32_t len, int rssi) {
    if (g_join != Join::None) {
        if (!memcmp(f + 16, g_bssid, 6)) g_last_beacon = timer_ms();
        return;
    }
    if (len < 36) return;
    bool secured = f[34] & 0x10, rsn_ok = false;
    uint32_t p = 36;
    const uint8_t* ssid = nullptr;
    int ssid_len = 0, channel = g_channel;
    while (p + 2 <= len) {
        uint32_t id = f[p], n = f[p + 1];
        if (p + 2 + n > len) break;
        if (id == 0) { ssid = f + p + 2; ssid_len = (int)n; }
        else if (id == 3 && n == 1) channel = f[p + 2];
        else if (id == 48) rsn_ok = rsn_usable(f + p + 2, n);
        p += 2 + n;
    }
    if (ssid && channel >= 1 && channel <= 13) remember((const char*)ssid, ssid_len, secured, rssi, f + 16, channel, rsn_ok);
}

uint8_t* tx_alloc() {
    int next = (g_tx_tail + 1) % kTxSlots;
    return next == g_tx_head ? nullptr : g_tx[g_tx_tail].data;
}
void tx_commit(uint16_t len, bool basic) {
    g_tx[g_tx_tail].len = len;
    g_tx[g_tx_tail].basic_rate = basic;
    g_tx_tail = (g_tx_tail + 1) % kTxSlots;
}

void tx_kick() {
    if (g_tx_busy) {
        volatile uint32_t* d = g_tx_desc;
        if (d[23] & 1) {
            if (d[15] & 1) g_tx_ok++;
            else if (++g_tx_fail <= 5) klog("ar9285: frame not acknowledged");
            g_tx_busy = false;
            g_tx_head = (g_tx_head + 1) % kTxSlots;
        } else if (timer_ms() - g_tx_time > kTxStuckMs) {
            klog("ar9285: transmit timed out");
            g_tx_busy = false;
            g_tx_head = (g_tx_head + 1) % kTxSlots;
        } else {
            return;
        }
    }
    if (g_tx_head == g_tx_tail) return;
    TxSlot& s = g_tx[g_tx_head];
    volatile uint32_t* d = g_tx_desc;
    for (int i = 0; i < 24; i++) d[i] = 0;
    d[1] = (uint32_t)(uintptr_t)s.data;
    d[2] = ((s.len + kTxPadForFcs) & 0xFFF) | (63u << 16) | kClrDestMask;
    d[3] = s.len | (s.data[4] & 1 ? kNoAck : 0);
    d[4] = s.basic_rate ? (4u << 16) | (4u << 20) : kDataTries;
    d[5] = s.basic_rate ? kRateMgmt | (kRateMgmt << 8) : kDataRates;
    d[9] = kChainSelAll;
    __sync_synchronize();
    wr(AR_QTXDP0, (uint32_t)(uintptr_t)d);
    wr(AR_Q_TXE, 1);
    g_tx_busy = true;
    g_tx_time = timer_ms();
}

void write_header(uint8_t* f, uint8_t fc0, uint8_t fc1, const uint8_t* a1, const uint8_t* a3) {
    f[0] = fc0;
    f[1] = fc1;
    f[2] = (uint8_t)kDuration;
    f[3] = kDuration >> 8;
    memcpy(f + 4, a1, 6);
    memcpy(f + 10, g_mac, 6);
    memcpy(f + 16, a3, 6);
    uint16_t seq = (uint16_t)(g_seq++ << 4);
    f[22] = (uint8_t)seq;
    f[23] = seq >> 8;
}

void send_mgmt(uint8_t subtype, const uint8_t* body, size_t n) {
    uint8_t* f = tx_alloc();
    if (!f) return;
    write_header(f, subtype, 0, g_bssid, g_bssid);
    memcpy(f + 24, body, n);
    tx_commit((uint16_t)(24 + n), true);
}

void ccmp_nonce(uint8_t nonce[13], uint8_t priority, const uint8_t* a2, uint64_t pn) {
    nonce[0] = priority;
    memcpy(nonce + 1, a2, 6);
    for (int i = 0; i < 6; i++) nonce[7 + i] = (uint8_t)(pn >> (40 - i * 8));
}

// Sends LLC/SNAP + payload to the access point, encrypted once the key is installed.
void send_data(const uint8_t* dest, uint16_t ethertype, const uint8_t* payload, size_t n, bool basic) {
    uint8_t* f = tx_alloc();
    if (!f || n + 64 > kMaxFrame) return;
    static uint8_t plain[kMaxFrame];
    static const uint8_t snap[6] = {0xAA, 0xAA, 3, 0, 0, 0};
    memcpy(plain, snap, 6);
    plain[6] = ethertype >> 8;
    plain[7] = (uint8_t)ethertype;
    memcpy(plain + 8, payload, n);
    size_t body = n + 8;
    write_header(f, 0x08, g_tk_set ? 0x41 : 0x01, g_bssid, dest);
    if (!g_tk_set) {
        memcpy(f + 24, plain, body);
        tx_commit((uint16_t)(24 + body), basic);
        return;
    }
    uint64_t pn = ++g_tx_pn;
    uint8_t* h = f + 24;
    h[0] = (uint8_t)pn; h[1] = (uint8_t)(pn >> 8); h[2] = 0; h[3] = 0x20;
    h[4] = (uint8_t)(pn >> 16); h[5] = (uint8_t)(pn >> 24); h[6] = (uint8_t)(pn >> 32); h[7] = (uint8_t)(pn >> 40);
    uint8_t aad[22], nonce[13];
    aad[0] = f[0] & 0x8F;
    aad[1] = f[1] & 0xC7;
    memcpy(aad + 2, f + 4, 18);
    aad[20] = f[22] & 0x0F;
    aad[21] = 0;
    ccmp_nonce(nonce, 0, g_mac, pn);
    crypto::ccmp_encrypt(g_tk, nonce, aad, 22, plain, body, f + 32);
    tx_commit((uint16_t)(32 + body + 8), basic);
}

void set_state(Join s) { g_join = s; g_state_time = timer_ms(); }

void leave(net::WifiStatus result) {
    g_join = Join::None;
    g_tk_set = false;
    for (bool& b : g_gtk_set) b = false;
    memset(g_bssid, 0, 6);
    g_aid = 0;
    g_ssid[0] = 0;
    g_status = result;
    g_tx_head = g_tx_tail = 0;
    g_tx_busy = false;
    stop_rx();
}

void send_auth() {
    static const uint8_t body[6] = {0, 0, 1, 0, 0, 0};
    send_mgmt(0xB0, body, sizeof(body));
}

void send_assoc() {
    uint8_t b[128];
    size_t p = 0;
    b[p++] = g_secured ? 0x11 : 0x01; b[p++] = 0;
    b[p++] = 10; b[p++] = 0;
    size_t ssid_len = strlen(g_ssid);
    b[p++] = 0; b[p++] = (uint8_t)ssid_len;
    memcpy(b + p, g_ssid, ssid_len); p += ssid_len;
    static const uint8_t rates[] = {1, 8, 0x82, 0x84, 0x8B, 0x96, 0x0C, 0x12, 0x18, 0x24, 50, 4, 0x30, 0x48, 0x60, 0x6C};
    memcpy(b + p, rates, sizeof(rates)); p += sizeof(rates);
    if (g_secured) { memcpy(b + p, kRsnIe, sizeof(kRsnIe)); p += sizeof(kRsnIe); }
    send_mgmt(0x00, b, p);
}

void link_ready() {
    set_state(Join::Up);
    g_last_beacon = timer_ms();
    g_status = net::WifiStatus::Connected;
    g_tx_ok = g_tx_fail = g_rx_hw_decrypt_err = g_rx_data = g_rx_no_key = g_rx_replay = g_rx_bad_mic = g_rx_delivered = g_rx_resync = 0;
    g_stats_time = timer_ms();
    g_stats_left = 12;
    klog("ar9285: connected to %s", g_ssid);
}

void send_eapol_key(const uint8_t* src, uint16_t info, const uint8_t* nonce, const uint8_t* data, size_t dlen) {
    uint8_t m[160];
    memset(m, 0, 99);
    m[0] = 1; m[1] = 3;
    uint16_t total = (uint16_t)(95 + dlen);
    m[2] = total >> 8; m[3] = (uint8_t)total;
    m[4] = 2;
    m[5] = info >> 8; m[6] = (uint8_t)info;
    memcpy(m + 9, src + 9, 8);                     // replay counter
    if (nonce) memcpy(m + 17, nonce, 32);
    m[97] = (uint8_t)(dlen >> 8); m[98] = (uint8_t)dlen;
    if (dlen) memcpy(m + 99, data, dlen);
    uint8_t mic[20];
    crypto::hmac_sha1(g_kck, 16, m, 99 + dlen, mic);
    memcpy(m + 81, mic, 16);
    send_data(g_bssid, 0x888E, m, 99 + dlen, true);
}

bool eapol_mic_ok(const uint8_t* m, size_t n) {
    static uint8_t copy[1024];
    if (n > sizeof(copy)) return false;
    memcpy(copy, m, n);
    memset(copy + 81, 0, 16);
    uint8_t mic[20];
    crypto::hmac_sha1(g_kck, 16, copy, n, mic);
    return !memcmp(mic, m + 81, 16);
}

uint8_t g_pending_tk[16];
void activate_tk() {
    g_tk.init(g_pending_tk);
    g_tk_set = true;
    g_tx_pn = 0;
    g_rx_pn = 0;
}

void derive_ptk() {
    uint8_t d[76], ptk[48];
    bool mac_first = memcmp(g_mac, g_bssid, 6) < 0, nonce_first = memcmp(g_snonce, g_anonce, 32) < 0;
    memcpy(d, mac_first ? g_mac : g_bssid, 6);
    memcpy(d + 6, mac_first ? g_bssid : g_mac, 6);
    memcpy(d + 12, nonce_first ? g_snonce : g_anonce, 32);
    memcpy(d + 44, nonce_first ? g_anonce : g_snonce, 32);
    crypto::prf_sha1(g_pmk, 32, "Pairwise key expansion", d, sizeof(d), ptk, sizeof(ptk));
    memcpy(g_kck, ptk, 16);
    memcpy(g_kek, ptk + 16, 16);
    memcpy(g_pending_tk, ptk + 32, 16);
}

// Finds the group key in decrypted key data and installs it.
bool install_gtk(const uint8_t* m, size_t n) {
    size_t dlen = (size_t)(m[97] << 8) | m[98];
    if (99 + dlen > n || dlen < 24 || (dlen & 7) || dlen > 512) return false;
    static uint8_t plain[512];
    if (!crypto::aes_unwrap(g_kek, m + 99, dlen, plain)) return false;
    size_t len = dlen - 8, p = 0;
    while (p + 2 <= len) {
        uint8_t id = plain[p], l = plain[p + 1];
        if (id == 0xDD && l == 0 && p + 2 == len) break;
        if (p + 2 + l > len) break;
        if (id == 0xDD && l >= 22 && !memcmp(plain + p + 2, "\x00\x0F\xAC\x01", 4)) {
            int key_id = plain[p + 6] & 3;
            g_gtk[key_id].init(plain + p + 8);
            g_gtk_set[key_id] = true;
            return true;
        }
        p += 2 + l;
    }
    return false;
}

void handle_eapol(const uint8_t* m, size_t n) {
    if (n < 99 || m[1] != 3 || m[4] != 2) return;
    uint16_t info = (uint16_t)((m[5] << 8) | m[6]);
    if ((info & 7) != KI_V2) { leave(net::WifiStatus::Unsupported); return; }
    if ((info & KI_PAIRWISE) && (info & KI_ACK) && !(info & KI_MIC)) {          // message 1
        if (g_join != Join::WaitM1 && g_join != Join::WaitM3) return;
        memcpy(g_anonce, m + 17, 32);
        if (g_join == Join::WaitM1) crypto::random_bytes(g_snonce, 32);
        derive_ptk();
        send_eapol_key(m, KI_V2 | KI_PAIRWISE | KI_MIC, g_snonce, kRsnIe, sizeof(kRsnIe));
        if (g_join == Join::WaitM1) set_state(Join::WaitM3);
        return;
    }
    if (!(info & KI_MIC) || !eapol_mic_ok(m, n)) return;
    if ((info & KI_PAIRWISE) && (info & KI_INSTALL)) {                          // message 3
        if (g_join != Join::WaitM3 && g_join != Join::Up) return;
        if (!(info & KI_ENCRYPTED) || !install_gtk(m, n)) { klog("ar9285: no usable group key"); return; }
        send_eapol_key(m, KI_V2 | KI_PAIRWISE | KI_MIC | KI_SECURE, nullptr, nullptr, 0);
        if (!g_tk_set) activate_tk();          // a resent message 3 must never reset the packet counter
        if (g_join != Join::Up) link_ready();
        return;
    }
    if (!(info & KI_PAIRWISE) && (info & KI_ENCRYPTED) && install_gtk(m, n))    // group rekey
        send_eapol_key(m, KI_V2 | KI_MIC | KI_SECURE, nullptr, nullptr, 0);
}

void handle_data(uint8_t* f, uint32_t len) {
    if (!(f[1] & 2) || (f[1] & 1) || memcmp(f + 10, g_bssid, 6)) return;      // must come from the AP
    if (f[0] & 0x40) return;                                                   // null data
    g_rx_data++;
    bool group = f[4] & 1;
    if (group && !memcmp(f + 16, g_mac, 6)) return;                            // our own broadcast, echoed
    bool qos = f[0] & 0x80;
    uint32_t hdr = qos ? 26 : 24, body = (hdr + 3) & ~3u;                      // the chip pads QoS headers
    if (len < body + 8) return;
    static uint8_t plain[kMaxFrame];
    const uint8_t* payload = f + body;
    uint32_t n = len - body;
    if (f[1] & 0x40) {
        if (n < 16) return;
        const uint8_t* h = f + body;
        int key_id = h[3] >> 6;
        const crypto::Aes128* key = group ? (g_gtk_set[key_id] ? &g_gtk[key_id] : nullptr) : (g_tk_set ? &g_tk : nullptr);
        if (!key) { g_rx_no_key++; return; }
        uint64_t pn = (uint64_t)h[0] | ((uint64_t)h[1] << 8) | ((uint64_t)h[4] << 16) | ((uint64_t)h[5] << 24) | ((uint64_t)h[6] << 32) | ((uint64_t)h[7] << 40);
        if (!group && pn <= g_rx_pn) { g_rx_replay++; return; }                // replay or retry
        uint8_t aad[24], nonce[13];
        aad[0] = f[0] & 0x8F;
        aad[1] = f[1] & 0xC7;
        memcpy(aad + 2, f + 4, 18);
        aad[20] = f[22] & 0x0F;
        aad[21] = 0;
        uint8_t priority = qos ? f[24] & 0x0F : 0;
        size_t alen = 22;
        if (qos) { aad[22] = priority; aad[23] = 0; alen = 24; }
        ccmp_nonce(nonce, priority, g_bssid, pn);
        if (!crypto::ccmp_decrypt(*key, nonce, aad, alen, h + 8, n - 16, plain)) { g_rx_bad_mic++; return; }
        if (!group) g_rx_pn = pn;
        payload = plain;
        n -= 16;
    } else if (g_secured) {
        // before the keys exist only the handshake may arrive in the clear
        if (n < 8 || payload[6] != 0x88 || payload[7] != 0x8E) return;
    }
    if (n < 8 || memcmp(payload, "\xAA\xAA\x03\x00\x00\x00", 6)) return;
    if (payload[6] == 0x88 && payload[7] == 0x8E) { handle_eapol(payload + 8, n - 8); return; }
    if (g_join != Join::Up || !g_deliver || n - 8 + 14 > kMaxFrame) return;
    memcpy(g_eth, f + 4, 6);
    memcpy(g_eth + 6, f + 16, 6);
    memcpy(g_eth + 12, payload + 6, n - 6);
    g_rx_delivered++;
    g_deliver(g_eth, n - 8 + 14);
}

void handle_mgmt(const uint8_t* f, uint32_t len) {
    if (memcmp(f + 10, g_bssid, 6) || memcmp(f + 4, g_mac, 6) || len < 26) return;
    uint8_t sub = f[0] & 0xF0;
    const uint8_t* b = f + 24;
    if (sub == 0xB0 && g_join == Join::Auth && len >= 30) {
        if (b[2] != 2) return;
        if (b[4] | b[5]) { klog("ar9285: authentication refused, status %u", b[4] | (b[5] << 8)); leave(net::WifiStatus::Failed); return; }
        g_tries = 0;
        set_state(Join::Assoc);
        send_assoc();
    } else if (sub == 0x10 && g_join == Join::Assoc && len >= 30) {
        uint32_t status = b[2] | (b[3] << 8);
        if (status) { klog("ar9285: association refused, status %u", status); leave(net::WifiStatus::Failed); return; }
        g_aid = (uint16_t)((b[4] | (b[5] << 8)) & 0x3FFF);
        wr(AR_BSS_ID1, g_bssid[4] | (g_bssid[5] << 8) | ((uint32_t)g_aid << 16));
        if (g_secured) set_state(Join::WaitM1);
        else link_ready();
    } else if (sub == 0xC0 || sub == 0xA0) {
        uint32_t reason = b[0] | (b[1] << 8);
        klog("ar9285: disconnected by the access point, reason %u", reason);
        bool handshake = g_join == Join::WaitM1 || g_join == Join::WaitM3;
        leave(handshake && (reason == 15 || reason == 2 || reason == 14 || reason == 23) ? net::WifiStatus::WrongPassword : net::WifiStatus::Failed);
    }
}

void handle_frame(uint8_t* f, uint32_t len, int rssi) {
    if (len < 24) return;
    uint8_t type = f[0] & 0x0C, sub = f[0] & 0xF0;
    if (type == 0 && (sub == 0x80 || sub == 0x50)) parse_beacon(f, len, rssi);
    else if (g_join == Join::None) return;
    else if (type == 0) handle_mgmt(f, len);
    else if (type == 8) handle_data(f, len);
}

void drain_rx() {
    for (int i = 0; i < kRxDescs; i++) {
        RxDesc* d = g_desc[g_rx_next];
        if (!(d->status[8] & RX_DONE)) {
            // if the ring overran, the chip has moved past this slot: continue at the next finished one
            int skip = 1;
            while (skip < kRxDescs && !(g_desc[(g_rx_next + skip) % kRxDescs]->status[8] & RX_DONE)) skip++;
            if (skip == kRxDescs) return;
            g_rx_resync++;
            g_rx_next = (g_rx_next + skip) % kRxDescs;
            d = g_desc[g_rx_next];
        }
        if (d->status[8] & RX_FRAME_OK) {
            g_frames++;
            int rssi = (int8_t)(d->status[4] >> 24);
            uint32_t len = d->status[1] & 0xFFF;
            if (len > 4) handle_frame(g_buf[g_rx_next], len - 4, rssi == -128 ? 0 : rssi);
        }
        else if (d->status[8] & RX_DECRYPT_ERR) g_rx_hw_decrypt_err++;
        arm_descriptor(g_rx_next);
        g_rx_next = (g_rx_next + 1) % kRxDescs;
    }
}

void finish_scan() {
    stop_rx();
    g_scanning = false;
    for (int i = 0; i < g_seen_count; i++) {
        int r = g_rssi[i];
        g_seen[i].signal = r > 40 ? 4 : r > 25 ? 3 : r > 12 ? 2 : 1;
    }
    for (int i = 1; i < g_seen_count; i++)               // strongest first
        for (int j = i; j > 0 && g_rssi[j] > g_rssi[j - 1]; j--) {
            net::WifiNetwork n = g_seen[j]; g_seen[j] = g_seen[j - 1]; g_seen[j - 1] = n;
            int8_t r = g_rssi[j]; g_rssi[j] = g_rssi[j - 1]; g_rssi[j - 1] = r;
            SeenInfo x = g_info[j]; g_info[j] = g_info[j - 1]; g_info[j - 1] = x;
        }
    klog("ar9285: scan done, %u frames received, %d networks", g_frames, g_seen_count);
}

bool tune_channel(int channel) {
    if (!tune(2407 + (uint32_t)channel * 5)) {
        klog("ar9285: could not tune channel %d", channel);
        finish_scan();
        return false;
    }
    g_dwell_end = timer_ms() + kDwellMs;
    return true;
}

void join_timers() {
    uint64_t now = timer_ms(), age = now - g_state_time;
    switch (g_join) {
        case Join::Auth:
        case Join::Assoc:
            if (age < kRetryMs) return;
            if (++g_tries >= kMaxTries) { klog("ar9285: no answer from the access point"); leave(net::WifiStatus::Failed); return; }
            g_state_time = now;
            if (g_join == Join::Auth) send_auth();
            else send_assoc();
            break;
        case Join::WaitM1:
            if (age > kHandshakeMs) { klog("ar9285: handshake never started"); leave(net::WifiStatus::Failed); }
            break;
        case Join::WaitM3:
            if (age > kHandshakeMs) { klog("ar9285: handshake not answered, wrong password?"); leave(net::WifiStatus::WrongPassword); }
            break;
        case Join::Up:
            if (now - g_last_beacon > kBeaconLossMs) { klog("ar9285: lost the access point"); leave(net::WifiStatus::Failed); break; }
            if (now - g_stats_time >= (g_stats_left > 0 ? 5000u : 30000u)) {   // often in the first minute online, then a steady trickle
                if (g_stats_left > 0) g_stats_left--;
                g_stats_time = now;
                klog("ar9285: tx ok %u fail %u | rx data %u hwerr %u nokey %u replay %u badmic %u delivered %u resync %u", g_tx_ok, g_tx_fail, g_rx_data,
                     g_rx_hw_decrypt_err, g_rx_no_key, g_rx_replay, g_rx_bad_mic, g_rx_delivered, g_rx_resync);
            }
            break;
        default: break;
    }
}

void nic_poll(void (*deliver)(const uint8_t*, size_t)) {
    g_deliver = deliver;
    if (g_scanning) {
        drain_rx();
        if (timer_ms() < g_dwell_end) return;
        if (g_channel == 13) { finish_scan(); return; }
        tune_channel(++g_channel);
        return;
    }
    if (g_join == Join::None) return;
    drain_rx();
    join_timers();
    if (g_join != Join::None) tx_kick();
}

void nic_send(const uint8_t* frame, size_t len) {
    if (g_join != Join::Up || len < 14) return;
    send_data(frame, (uint16_t)((frame[12] << 8) | frame[13]), frame + 14, len - 14, false);
    tx_kick();
}

bool nic_link() { return g_join == Join::Up; }

void wifi_scan() {
    if (g_scanning || g_join != Join::None) return;
    g_seen_count = 0;
    g_frames = 0;
    g_channel = 1;
    g_scanning = true;
    tune_channel(1);
}
bool wifi_scanning() { return g_scanning; }
int wifi_results(net::WifiNetwork* out, int max) {
    int n = g_seen_count < max ? g_seen_count : max;
    for (int i = 0; i < n; i++) out[i] = g_seen[i];
    return n;
}
// As in every WPA client, exactly 64 hex digits are the key itself rather than a passphrase.
bool key_from_hex(const char* s, uint8_t key[32]) {
    if (strlen(s) != 64) return false;
    uint8_t out[32] = {};
    for (int i = 0; i < 64; i++) {
        char c = s[i];
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) return false;
        out[i / 2] = (uint8_t)(out[i / 2] << 4 | v);
    }
    memcpy(key, out, 32);
    return true;
}

void wifi_connect(const char* ssid, const char* password) {
    if (g_join != Join::None) leave(net::WifiStatus::Idle);
    if (g_scanning) finish_scan();
    int found = -1;
    for (int i = 0; i < g_seen_count; i++)
        if (!strcmp(g_seen[i].ssid, ssid)) found = i;
    if (found < 0) { g_status = net::WifiStatus::Failed; return; }
    g_secured = g_seen[found].secured;
    if (g_secured && !g_info[found].rsn_ok) {
        klog("ar9285: %s does not offer WPA2-PSK with CCMP", ssid);
        g_status = net::WifiStatus::Unsupported;
        return;
    }
    strlcpy(g_ssid, ssid, sizeof(g_ssid));
    memcpy(g_bssid, g_info[found].bssid, 6);
    g_join_channel = g_info[found].channel;
    g_aid = 0;
    g_status = net::WifiStatus::Connecting;
    g_tk_set = false;
    if (g_secured && !key_from_hex(password, g_pmk)) crypto::wpa_psk(password, (const uint8_t*)g_ssid, strlen(g_ssid), g_pmk);
    g_tries = 0;
    set_state(Join::Auth);                 // so start_rx opens the filter for us
    if (!tune(2407 + (uint32_t)g_join_channel * 5)) { leave(net::WifiStatus::Failed); return; }
    g_last_beacon = timer_ms();
    klog("ar9285: joining %s on channel %d", g_ssid, g_join_channel);
    send_auth();
    tx_kick();
}
void wifi_disconnect() {
    if (g_join == Join::Up || g_join == Join::WaitM3) {
        static const uint8_t reason[2] = {3, 0};
        send_mgmt(0xC0, reason, 2);
        tx_kick();
    }
    leave(net::WifiStatus::Idle);
}
net::WifiStatus wifi_status() { return g_status; }
const char* wifi_connected() { return g_join == Join::Up ? g_ssid : ""; }

const net::WifiDriver kWifi = {wifi_scan, wifi_scanning, wifi_results, wifi_connect, wifi_disconnect, wifi_status, wifi_connected};

bool probe(const PciDevice* d) {
    if (d->bar[0] & 1) { klog("ar9285: BAR0 is not memory-mapped"); return false; }
    if ((d->bar[0] & 6) == 4 && d->bar[1]) { klog("ar9285: BAR0 is above 4 GiB"); return false; }
    pci_enable(d);
    g_mmio = (volatile uint8_t*)(uintptr_t)(d->bar[0] & ~0xFu);
    if (!power_on_reset()) { klog("ar9285: chip did not come out of reset"); return false; }

    uint32_t srev = rd(AR_SREV);
    uint32_t version = (srev & AR_SREV_VERSION2) >> AR_SREV_TYPE2_S;
    uint32_t revision = (srev & AR_SREV_REVISION2) >> 8;
    if ((srev & 0xFF) != 0xFF || version != AR_SREV_VERSION_9285) {
        klog("ar9285: unexpected silicon revision %08x", srev);
        return false;
    }
    if (revision < AR_SREV_REVISION_9285_12) { klog("ar9285: revision %u is too old for these tables", revision); return false; }

    uint16_t magic = 0;
    if (!eeprom_read(0, &magic)) { klog("ar9285: EEPROM not responding"); return false; }
    bool swapped = magic == ((kEepromMagic >> 8) | ((kEepromMagic & 0xFF) << 8));
    if (magic != kEepromMagic && !swapped) { klog("ar9285: bad EEPROM magic %04x", magic); return false; }
    for (int i = 0; i < kEepWords; i++) {
        uint16_t v;
        if (!eeprom_read(kEep4kStart + i, &v)) { klog("ar9285: EEPROM read failed"); return false; }
        if (swapped) v = (uint16_t)((v >> 8) | (v << 8));
        g_eep[i * 2] = v & 0xFF;
        g_eep[i * 2 + 1] = v >> 8;
    }
    memcpy(g_mac, g_eep + 12, 6);

    for (int i = 0; i < kRxDescs; i++) {
        g_desc[i] = (RxDesc*)kmalloc(sizeof(RxDesc));
        g_buf[i] = (uint8_t*)kmalloc(kRxBufBytes);
        if (!g_desc[i] || !g_buf[i]) { klog("ar9285: out of memory"); return false; }
    }
    for (int i = 0; i < kRxDescs; i++) {
        g_desc[i]->link = (uint32_t)(uintptr_t)g_desc[(i + 1) % kRxDescs];
        g_desc[i]->data = (uint32_t)(uintptr_t)g_buf[i];
    }
    g_tx = (TxSlot*)kmalloc(sizeof(TxSlot) * kTxSlots);
    g_tx_desc = (volatile uint32_t*)kmalloc(24 * 4);
    if (!g_tx || !g_tx_desc) { klog("ar9285: out of memory"); return false; }
    klog("ar9285: found rev %u, MAC %02x:%02x:%02x:%02x:%02x:%02x, EEPROM minor %u, modal version %u", revision, g_mac[0], g_mac[1],
         g_mac[2], g_mac[3], g_mac[4], g_mac[5], (g_eep[4] | (g_eep[5] << 8)) & 0xFFF, modal(37));
    g_nic.name = "Atheros AR9285 Wi-Fi";
    memcpy(g_nic.mac, g_mac, 6);
    g_nic.send = nic_send;
    g_nic.link_up = nic_link;
    g_nic.poll = nic_poll;
    net::register_nic(&g_nic);
    net::wifi_register(&kWifi);
    return true;
}

const DriverPciId kIds[] = {{0x168C, 0x002B}};

}  // namespace

extern "C" {
extern const DriverInfo klein_driver;
const DriverInfo klein_driver = {KLEIN_DRIVER_ABI, "Atheros AR9285", kIds, 1, probe};
}
