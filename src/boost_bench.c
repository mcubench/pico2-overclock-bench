/*
 * boost_bench - RP2350 / Raspberry Pi Pico 2
 *
 * Measures the real cost of temporarily boosting clk_sys and the core
 * regulator from 150 MHz / 1.10 V to 500 MHz / 1.60 V and back again.
 *
 * Results go to USB CDC serial (and UART on GP0/GP1 at 115200).
 *
 *   !! This deliberately drives the core rail ~45% above its rated       !!
 *   !! maximum. Run it on a board you can afford to lose. Lower          !!
 *   !! BOOST_VREG_LEVEL if you want a gentler test.                      !!
 *
 * Two things worth knowing before reading the code:
 *
 *  1. 150 MHz and 500 MHz are both derived from the SAME 1500 MHz VCO
 *     (1500/(5*2) and 1500/(3*1)), so the clock can be changed by
 *     rewriting the PLL post-dividers alone - the VCO never unlocks.
 *     That "fast path" is benchmarked against set_sys_clock_khz().
 *
 *  2. The only measurement that is actually board-specific is how long
 *     the core rail needs before the boosted clock is safe. Test 4
 *     finds that empirically by checking computed results, which is the
 *     only trustworthy way to do it from inside firmware.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"
#include "pico/stdio_usb.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/vreg.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "hardware/timer.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/powman.h"
#include "hardware/structs/clocks.h"
#include "hardware/structs/pll.h"
#include "hardware/sha256.h"

/* ================= configuration ================= */

#ifndef BASE_KHZ
#define BASE_KHZ            150000u
#endif
#ifndef BOOST_KHZ
#define BOOST_KHZ           500000u
#endif

#define BASE_PD1            5u      /* 1500 / (5*2) = 150 MHz */
#define BASE_PD2            2u
#define BOOST_PD1           3u      /* 1500 / (3*1) = 500 MHz */
#define BOOST_PD2           1u

#define BASE_VREG_LEVEL     VREG_VOLTAGE_1_10
#define BOOST_VREG_LEVEL    VREG_VOLTAGE_1_60

#define MARKER_PIN          15      /* scope probe: high while boosted */
#define RAIL_DECAY_MS       30      /* let the rail fall so each ramp is real */

#define WBUF_WORDS          2048u   /* 8 KiB working set in SRAM */
#define SHABUF_WORDS        1024u   /* 4 KiB message for the SHA-256 block */
#define SHA_ITERS           64u     /* hashes per phase                   */
#define SHA_SWEEP_BYTES     1024u   /* short hash used inside the sweep   */
#define SHA_SETTLE_US       4000u   /* conservative settle for the SHA test */
#define CHECK_ROUNDS        3u
#define PERF_ROUNDS         200u

#define PRIM_OF(d1, d2) (((uint32_t)(d1) << PLL_PRIM_POSTDIV1_LSB) | \
                         ((uint32_t)(d2) << PLL_PRIM_POSTDIV2_LSB))
#define BASE_PRIM   PRIM_OF(BASE_PD1, BASE_PD2)
#define BOOST_PRIM  PRIM_OF(BOOST_PD1, BOOST_PD2)

/* watchdog scratch survives a crash-reboot */
#define WD_MAGIC            0xB005751Cu
#define wd_magic            (watchdog_hw->scratch[0])
#define wd_phase            (watchdog_hw->scratch[1])
#define wd_detail           (watchdog_hw->scratch[2])
#define wd_min_bad_settle   (watchdog_hw->scratch[3])

enum { PH_IDLE = 0, PH_XIP, PH_PRIM, PH_VOUT, PH_SHA, PH_SWEEP, PH_RT, PH_THRU, PH_MAX };
static const char *phase_name[PH_MAX] = {
    "idle", "XIP timing probe", "component costs", "VOUT_OK timing",
    "SHA-256 accelerator", "settle-delay sweep", "round-trip", "throughput"
};

/* ================= state ================= */

static uint32_t base_qmi_timing, base_qmi_div, base_qmi_rxd;
static uint32_t boost_qmi_div,  boost_qmi_rxd;
static bool     xip_retimed = false;   /* do we have a known-good boosted QMI? */

static uint32_t wbuf[WBUF_WORDS];
static uint32_t golden_hash, golden_crc1k, golden_crc4k;

static uint8_t  probe_ok[3][8];        /* [div candidate][rxdelay] */

static uint32_t        shabuf[SHABUF_WORDS];
static sha256_result_t golden_sha4k, golden_sha1k;

/* Known-answer digests, computed off-device from the exact byte pattern
 * sha_fill() produces. These prove the hardware block is right, not merely
 * self-consistent with an earlier on-device run. */
static const uint8_t kat_sha4k[32] = {
    0xa5, 0xce, 0x8d, 0x31, 0x8f, 0xdb, 0x51, 0xf7,
    0xca, 0x57, 0xaa, 0xff, 0x5b, 0x77, 0xd8, 0x3e,
    0x5b, 0x80, 0x4c, 0xb9, 0xed, 0x3e, 0x0e, 0xfe,
    0x04, 0x50, 0x06, 0xe4, 0x40, 0xe8, 0xcd, 0xc5
};
static const uint8_t kat_sha1k[32] = {
    0x9b, 0x65, 0xe1, 0xf9, 0x4c, 0xcf, 0xa4, 0x4c,
    0x16, 0x76, 0xdb, 0x69, 0x5a, 0xe1, 0x86, 0xb5,
    0x18, 0xa6, 0x80, 0x2a, 0xda, 0x58, 0xb8, 0x48,
    0xf9, 0xaf, 0xab, 0x98, 0xc5, 0x5f, 0xea, 0x45
};

/* Free-running 1 us timer, fed from clk_ref/xosc. Stays correct across
 * every clk_sys change, which is exactly why it is the instrument here. */
static inline uint32_t tnow(void) { return timer_hw->timerawl; }

static void pr_ns(uint32_t ns) {
    printf("%lu.%03lu us", (unsigned long)(ns / 1000u), (unsigned long)(ns % 1000u));
}

/* ================= RAM-resident primitives ================= */

/* Changing flash timing must not itself be fetched from flash. The dummy
 * XIP read is required by the datasheet when raising CLKDIV ahead of a
 * clk_sys increase, so the divisor is in effect before the clock moves. */
static void __no_inline_not_in_flash_func(set_qmi)(uint32_t clkdiv, uint32_t rxdelay) {
    hw_write_masked(&qmi_hw->m[0].timing,
                    (clkdiv  << QMI_M0_TIMING_CLKDIV_LSB) |
                    (rxdelay << QMI_M0_TIMING_RXDELAY_LSB),
                    QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS);
    (void)*(volatile uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
    __dmb();
}

/* Fast clk_sys change: park the glitchless mux on clk_ref, rewrite the PLL
 * post-dividers, park back. No VCO relock, no PLL block reset. */
static void __no_inline_not_in_flash_func(fast_set_prim)(uint32_t prim) {
    hw_clear_bits(&clocks_hw->clk[clk_sys].ctrl, CLOCKS_CLK_SYS_CTRL_SRC_BITS);
    while (!(clocks_hw->clk[clk_sys].selected & 1u)) tight_loop_contents();

    hw_set_bits(&pll_sys_hw->pwr, PLL_PWR_POSTDIVPD_BITS);
    pll_sys_hw->prim = prim;
    hw_clear_bits(&pll_sys_hw->pwr, PLL_PWR_POSTDIVPD_BITS);

    hw_set_bits(&clocks_hw->clk[clk_sys].ctrl, CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX);
    while (!(clocks_hw->clk[clk_sys].selected & 2u)) tight_loop_contents();
}

static void __no_inline_not_in_flash_func(seed_buf)(void) {
    uint32_t v = 0x12345678u;
    for (uint32_t i = 0; i < WBUF_WORDS; i++) { v = v * 1664525u + 1013904223u; wbuf[i] = v; }
}

/* Multiply- and memory-heavy, fully deterministic. Chosen because long
 * multiply chains are the usual critical path that fails first when the
 * core is clocked past what its voltage supports. */
static uint32_t __no_inline_not_in_flash_func(workload)(uint32_t rounds) {
    uint32_t h = 0x9E3779B9u;
    for (uint32_t r = 0; r < rounds; r++) {
        for (uint32_t i = 0; i < WBUF_WORDS; i++) {
            uint32_t v = wbuf[i];
            v ^= h;
            v *= 0x85EBCA6Bu;
            v ^= v >> 13;
            v += v << 3;
            v ^= v >> 7;
            v *= 0xC2B2AE35u;
            wbuf[i] = v;
            h = (h ^ v) * 0x27D4EB2Fu;
            h ^= h >> 15;
        }
    }
    return h;
}

/* CRC32 of a flash window, read through the uncached XIP alias so the QSPI
 * interface is genuinely exercised rather than served from the XIP cache. */
static uint32_t __no_inline_not_in_flash_func(flash_crc)(uint32_t nbytes) {
    const volatile uint8_t *p = (const volatile uint8_t *)XIP_NOCACHE_NOALLOC_BASE;
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < nbytes; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return ~crc;
}

/* Deterministic message for the SHA-256 block. Never mutated, so every
 * hash of it must produce the same digest whatever the clock is doing. */
static void __no_inline_not_in_flash_func(sha_fill)(void) {
    uint32_t v = 0xDEADBEEFu;
    for (uint32_t i = 0; i < SHABUF_WORDS; i++) { v = v * 1103515245u + 12345u; shabuf[i] = v; }
}

typedef union { uint32_t w[16]; uint8_t b[64]; } sha_blk_t;

/* One complete SHA-256 over nbytes (a multiple of 64) using the RP2350
 * accelerator, including the final padding block, so the digest is a real
 * SHA-256 and can be checked against a known answer.
 *
 * With BSWAP enabled the message byte order is simply the buffer's byte
 * order in memory. The ready-poll before every word mirrors what the SDK
 * does; writing early sets ERR_WDATA_NOT_RDY and voids the digest. */
static uint32_t __no_inline_not_in_flash_func(sha_hw_hash)(const uint32_t *data,
                                                           uint32_t nbytes,
                                                           sha256_result_t *out) {
    sha256_err_not_ready_clear();
    sha256_set_dma_size(4);
    sha256_set_bswap(true);
    sha256_start();

    uint32_t nwords = nbytes / 4u;
    for (uint32_t i = 0; i < nwords; i++) {
        sha256_wait_ready_blocking();
        sha256_put_word(data[i]);
    }

    sha_blk_t pad;
    for (int i = 0; i < 16; i++) pad.w[i] = 0;
    pad.b[0] = 0x80u;
    uint64_t bits = (uint64_t)nbytes * 8u;
    for (int i = 0; i < 8; i++) pad.b[63 - i] = (uint8_t)(bits >> (8 * i));
    for (int i = 0; i < 16; i++) {
        sha256_wait_ready_blocking();
        sha256_put_word(pad.w[i]);
    }

    /* SUM_VLD resets to 1, so polling it straight after the final write can
     * hand back the previous block's digest. Waiting for WDATA_RDY first
     * means compression of the last block has finished. Bounded, because a
     * benchmark should never be able to hang the board. */
    uint32_t t_guard = tnow();
    while (!sha256_is_ready() && (tnow() - t_guard) < 2000u) tight_loop_contents();
    sha256_wait_valid_blocking();
    /* inlined equivalent of sha256_get_result(out, SHA256_BIG_ENDIAN); the
     * SDK version lives in flash and we want this loop entirely in SRAM */
    for (int i = 0; i < 8; i++) out->words[i] = __builtin_bswap32(sha256_hw->sum[i]);
    return sha256_err_not_ready() ? 1u : 0u;   /* non-zero = feed underrun */
}

static bool __no_inline_not_in_flash_func(digest_eq)(const sha256_result_t *a, const uint8_t *b) {
    for (int i = 0; i < 32; i++) if (a->bytes[i] != b[i]) return false;
    return true;
}

static void print_digest(const sha256_result_t *d) {
    for (int i = 0; i < 32; i++) printf("%02x", d->bytes[i]);
}

/* ================= transition paths ================= */

typedef enum { PATH_SDK = 0, PATH_FAST = 1 } path_t;
static const char *path_name[2] = { "SDK set_sys_clock_khz", "postdiv-only (RAM)" };

static inline void clk_to_boost(path_t p) {
    if (p == PATH_SDK) set_sys_clock_khz(BOOST_KHZ, true);
    else { fast_set_prim(BOOST_PRIM); clock_set_reported_hz(clk_sys, BOOST_KHZ * 1000u); }
}
static inline void clk_to_base(path_t p) {
    if (p == PATH_SDK) set_sys_clock_khz(BASE_KHZ, true);
    else { fast_set_prim(BASE_PRIM); clock_set_reported_hz(clk_sys, BASE_KHZ * 1000u); }
}

/* voltage -> settle -> flash timing -> clock */
static inline void boost_enter(path_t p, uint32_t settle_us) {
    gpio_put(MARKER_PIN, 1);
    vreg_set_voltage(BOOST_VREG_LEVEL);
    if (settle_us) busy_wait_us_32(settle_us);
    if (xip_retimed) set_qmi(boost_qmi_div, boost_qmi_rxd);
    clk_to_boost(p);
}
/* clock -> flash timing -> voltage */
static inline void boost_exit(path_t p) {
    clk_to_base(p);
    if (xip_retimed) set_qmi(base_qmi_div, base_qmi_rxd);
    vreg_set_voltage(BASE_VREG_LEVEL);
    gpio_put(MARKER_PIN, 0);
}

/* ================= test 1: QMI timing that survives the boost ========= */

/* Runs entirely from SRAM with interrupts off, so a wrong RXDELAY can only
 * corrupt the CRC data reads - it can never corrupt instruction fetch.
 * That is what makes it safe to search blind. */
static void __no_inline_not_in_flash_func(xip_probe)(uint32_t want_div, uint32_t golden) {
    uint32_t irq = save_and_disable_interrupts();

    set_qmi(want_div + 2, 4);          /* conservative; nothing depends on it */
    fast_set_prim(BOOST_PRIM);

    for (uint32_t d = 0; d < 3; d++) {
        for (uint32_t r = 0; r < 8; r++) {
            set_qmi(want_div + d, r);
            uint32_t c1 = flash_crc(1024);
            uint32_t c2 = flash_crc(1024);
            probe_ok[d][r] = (c1 == golden && c2 == golden) ? 1u : 0u;
        }
    }

    set_qmi(want_div + 2, 4);
    fast_set_prim(BASE_PRIM);
    set_qmi(base_qmi_div, base_qmi_rxd);
    restore_interrupts(irq);
}

static void test_xip_timing(void) {
    printf("\n== 1. Flash (QMI) timing at %lu MHz ==\n", (unsigned long)(BOOST_KHZ / 1000));
    wd_phase = PH_XIP;

    printf("  boot value M0_TIMING=0x%08lx -> CLKDIV=%lu RXDELAY=%lu (QSPI %lu MHz at base)\n",
           (unsigned long)base_qmi_timing, (unsigned long)base_qmi_div,
           (unsigned long)base_qmi_rxd, (unsigned long)(BASE_KHZ / 1000 / base_qmi_div));

    uint32_t want = (base_qmi_div * BOOST_KHZ + BASE_KHZ - 1) / BASE_KHZ;
    if (want < 2) want = 2;
    if (want > 250) want = 250;

    vreg_set_voltage(BOOST_VREG_LEVEL);
    busy_wait_ms(20);
    xip_probe(want, golden_crc1k);
    vreg_set_voltage(BASE_VREG_LEVEL);
    busy_wait_ms(RAIL_DECAY_MS);

    int best_d = -1, lo = 0, hi = 0, width = 0;
    for (int d = 0; d < 3; d++) {
        int l = -1, h = -1;
        for (int r = 0; r < 8; r++) {
            if (probe_ok[d][r]) { if (l < 0) l = r; h = r; }
        }
        printf("  CLKDIV=%-3lu (QSPI %2lu MHz)  RXDELAY ",
               (unsigned long)(want + d), (unsigned long)(BOOST_KHZ / 1000 / (want + d)));
        for (int r = 0; r < 8; r++) putchar(probe_ok[d][r] ? '.' : 'x');
        printf("   %s\n", (l >= 0) ? "" : "(no working value)");
        if (l >= 0 && (h - l + 1) > width) { width = h - l + 1; best_d = d; lo = l; hi = h; }
    }
    printf("  ('.' = correct CRC, 'x' = wrong, RXDELAY 0 on the left)\n");

    if (best_d >= 0) {
        boost_qmi_div = want + (uint32_t)best_d;
        boost_qmi_rxd = (uint32_t)((lo + hi) / 2);
        xip_retimed = true;
        printf("  chosen: CLKDIV=%lu RXDELAY=%lu (window %d..%d, centred)\n",
               (unsigned long)boost_qmi_div, (unsigned long)boost_qmi_rxd, lo, hi);
    } else {
        xip_retimed = false;
        printf("  ** no working RXDELAY found. Flash data reads while boosted are\n");
        printf("     unreliable; SDK-path timing tests will be skipped.\n");
    }
    wd_phase = PH_IDLE;
}

/* In the flash build, ANY boosted test resumes flash-resident code at
 * 500 MHz, so all of them need a known-good QMI retime first. In the
 * copy_to_ram build nothing is fetched from flash and it is always fine. */
static bool boosted_exec_ok(void) {
#if PICO_COPY_TO_RAM
    return true;
#else
    return xip_retimed;
#endif
}

/* ================= test 2: component costs ================= */

static uint32_t ns_vreg_write, ns_qmi_write;
static uint32_t ns_up[2], ns_dn[2];
static bool     have_path[2] = { false, true };

static void test_primitives(void) {
    const uint32_t N = 500;
    uint32_t t0, acc;

    printf("\n== 2. Component costs (rail held high - no settling in these) ==\n");
    wd_phase = PH_PRIM;

    /* POWMAN write plus its UPDATE_IN_PROGRESS handshake. NOT rail settling. */
    vreg_set_voltage(VREG_VOLTAGE_1_15);
    t0 = tnow();
    for (uint32_t i = 0; i < N; i++)
        vreg_set_voltage((i & 1) ? VREG_VOLTAGE_1_15 : VREG_VOLTAGE_1_20);
    acc = tnow() - t0;
    vreg_set_voltage(BASE_VREG_LEVEL);
    ns_vreg_write = (uint32_t)((uint64_t)acc * 1000u / N);
    printf("  vreg_set_voltage() register handshake : ");
    pr_ns(ns_vreg_write); printf("\n");

    t0 = tnow();
    for (uint32_t i = 0; i < N; i++) set_qmi(base_qmi_div, base_qmi_rxd);
    acc = tnow() - t0;
    ns_qmi_write = (uint32_t)((uint64_t)acc * 1000u / N);
    printf("  set_qmi() write + dummy XIP read      : ");
    pr_ns(ns_qmi_write); printf("\n");

    have_path[PATH_SDK]  = boosted_exec_ok();
    have_path[PATH_FAST] = boosted_exec_ok();

    vreg_set_voltage(BOOST_VREG_LEVEL);
    busy_wait_ms(20);
    if (xip_retimed) set_qmi(boost_qmi_div, boost_qmi_rxd);

    for (int p = 0; p < 2; p++) {
        if (!have_path[p]) {
            printf("  %-22s 150<->500            : skipped, no safe XIP timing\n",
                   path_name[p]);
            continue;
        }
        const uint32_t M = (p == PATH_SDK) ? 200u : 2000u;
        uint32_t up = 0, dn = 0;
        wd_detail = (uint32_t)p;
        uint32_t irq = save_and_disable_interrupts();
        for (uint32_t i = 0; i < M; i++) {
            uint32_t a = tnow(); clk_to_boost((path_t)p);
            uint32_t b = tnow(); clk_to_base((path_t)p);
            uint32_t c = tnow();
            up += b - a; dn += c - b;
        }
        restore_interrupts(irq);
        ns_up[p] = (uint32_t)((uint64_t)up * 1000u / M);
        ns_dn[p] = (uint32_t)((uint64_t)dn * 1000u / M);
        printf("  %-22s 150->500             : ", path_name[p]); pr_ns(ns_up[p]);
        printf("\n  %-22s 500->150             : ", "");          pr_ns(ns_dn[p]);
        printf("\n");
    }

    if (xip_retimed) set_qmi(base_qmi_div, base_qmi_rxd);
    vreg_set_voltage(BASE_VREG_LEVEL);
    busy_wait_ms(RAIL_DECAY_MS);
    wd_phase = PH_IDLE;
}

/* ================= test 3: what the regulator reports ================= */

static void test_vout_ok(void) {
    const uint32_t K = 16;
    uint32_t sum = 0, mn = 0xFFFFFFFFu, mx = 0, seen = 0;

    printf("\n== 3. POWMAN VREG_STS.VOUT_OK on a 1.10 -> 1.60 V step ==\n");
    wd_phase = PH_VOUT;

    for (uint32_t k = 0; k < K; k++) {
        vreg_set_voltage(BASE_VREG_LEVEL);
        busy_wait_ms(RAIL_DECAY_MS);

        uint32_t irq = save_and_disable_interrupts();
        uint32_t t0 = tnow();
        vreg_set_voltage(BOOST_VREG_LEVEL);
        bool dropped = false;
        uint32_t t_ok = 0;
        while ((tnow() - t0) < 50000u) {
            if (!(powman_hw->vreg_sts & POWMAN_VREG_STS_VOUT_OK_BITS)) dropped = true;
            else if (dropped) { t_ok = tnow() - t0; break; }
        }
        restore_interrupts(irq);

        if (dropped && t_ok) {
            seen++; sum += t_ok;
            if (t_ok < mn) mn = t_ok;
            if (t_ok > mx) mx = t_ok;
        }
    }
    vreg_set_voltage(BASE_VREG_LEVEL);
    busy_wait_ms(RAIL_DECAY_MS);

    if (!seen) {
        printf("  VOUT_OK never deasserted - the comparator does not treat this\n");
        printf("  step as out-of-regulation, so it is useless as a settle signal.\n");
        printf("  Test 4 is the one that tells you the real number.\n");
    } else {
        printf("  low->high after VSEL write in %lu/%lu steps: min %lu / mean %lu / max %lu us\n",
               (unsigned long)seen, (unsigned long)K, (unsigned long)mn,
               (unsigned long)(sum / seen), (unsigned long)mx);
        printf("  NB: 'in regulation' per the comparator, not 'settled and quiet'.\n");
    }
    wd_phase = PH_IDLE;
}


/* ================= test 4: SHA-256 accelerator before/during/after ==== */

typedef struct {
    const char *label;
    uint32_t    mhz;
    uint32_t    us_per_hash;      /* in nanoseconds, actually */
    uint32_t    kb_per_s;
    uint32_t    cyc_per_byte_x10;
    uint32_t    bad;              /* digest mismatches            */
    uint32_t    underrun;         /* ERR_WDATA_NOT_RDY occurrences */
    bool        ran;
    sha256_result_t digest;
} sha_phase_t;

static sha_phase_t sha_ph[3];
static bool sha_kat_ok = false;

/* Hash SHA_ITERS times, verify every digest, and time the lot. Runs with
 * interrupts off so the timing is not polluted by USB servicing. */
static void __no_inline_not_in_flash_func(sha_run_phase)(sha_phase_t *ph,
                                                        const char *label,
                                                        uint32_t mhz) {
    sha256_result_t d;
    uint32_t bad = 0, un = 0;

    uint32_t irq = save_and_disable_interrupts();
    uint32_t t0 = tnow();
    for (uint32_t i = 0; i < SHA_ITERS; i++) {
        un += sha_hw_hash(shabuf, SHABUF_WORDS * 4u, &d);
        if (!digest_eq(&d, golden_sha4k.bytes)) bad++;
    }
    uint32_t el = tnow() - t0;
    restore_interrupts(irq);

    uint64_t bytes = (uint64_t)SHABUF_WORDS * 4u * SHA_ITERS;
    ph->label   = label;
    ph->mhz     = mhz;
    ph->ran     = true;
    ph->bad     = bad;
    ph->underrun = un;
    ph->digest  = d;
    ph->us_per_hash    = (uint32_t)((uint64_t)el * 1000u / SHA_ITERS);   /* ns */
    ph->kb_per_s       = el ? (uint32_t)(bytes * 1000u / el) : 0;        /* B/ms = KB/s */
    ph->cyc_per_byte_x10 = el ? (uint32_t)(((uint64_t)el * mhz * 10u) / bytes) : 0;
}

static void print_sha_row(const sha_phase_t *ph) {
    if (!ph->ran) { printf("  %-22s skipped\n", ph->label); return; }
    printf("  %-22s %3lu MHz  ", ph->label, (unsigned long)ph->mhz);
    pr_ns(ph->us_per_hash);
    printf("/hash  %lu.%02lu MB/s  %lu.%lu cyc/B  %s",
           (unsigned long)(ph->kb_per_s / 1000u), (unsigned long)((ph->kb_per_s / 10u) % 100u),
           (unsigned long)(ph->cyc_per_byte_x10 / 10u), (unsigned long)(ph->cyc_per_byte_x10 % 10u),
           ph->bad ? "DIGEST WRONG" : "all digests ok");
    if (ph->underrun) printf("  [%lu feed underruns]", (unsigned long)ph->underrun);
    printf("\n");
}

static void test_sha256(void) {
    printf("\n== 4. Hardware SHA-256 accelerator ==\n");
    printf("   %lu-byte message, %lu hashes per phase, digest checked every time.\n",
           (unsigned long)(SHABUF_WORDS * 4u), (unsigned long)SHA_ITERS);
    wd_phase = PH_SHA;

    memset(sha_ph, 0, sizeof(sha_ph));

    /* ---- before: base clock, base voltage ---- */
    wd_detail = 0;
    sha_run_phase(&sha_ph[0], "before boost", BASE_KHZ / 1000u);

    if (!boosted_exec_ok()) {
        printf("   boosted phase skipped: no safe XIP timing in this build.\n");
        print_sha_row(&sha_ph[0]);
        wd_phase = PH_IDLE;
        return;
    }

    /* ---- during: boosted, using a deliberately generous settle ---- */
    wd_detail = 1;
    watchdog_enable(4000, 1);
    vreg_set_voltage(BASE_VREG_LEVEL);
    busy_wait_ms(RAIL_DECAY_MS);
    watchdog_update();
    boost_enter(PATH_FAST, SHA_SETTLE_US);
    sha_run_phase(&sha_ph[1], "during boost", BOOST_KHZ / 1000u);
    boost_exit(PATH_FAST);
    watchdog_disable();

    /* ---- after: back at base, rail given time to come down ---- */
    wd_detail = 2;
    busy_wait_ms(RAIL_DECAY_MS);
    sha_run_phase(&sha_ph[2], "after boost", BASE_KHZ / 1000u);

    for (int i = 0; i < 3; i++) print_sha_row(&sha_ph[i]);

    printf("   digest: ");
    print_digest(&sha_ph[0].digest);
    printf("\n   known answer %s\n", sha_kat_ok ? "MATCHES" : "DOES NOT MATCH (see startup note)");

    if (sha_ph[1].ran && sha_ph[1].us_per_hash && sha_ph[0].us_per_hash) {
        uint32_t sx = (uint32_t)((uint64_t)sha_ph[0].us_per_hash * 100u / sha_ph[1].us_per_hash);
        printf("   boosted speedup %lu.%02lux on the SHA block\n",
               (unsigned long)(sx / 100), (unsigned long)(sx % 100));
    }
    if (sha_ph[2].ran && sha_ph[2].bad)
        printf("   ** the block is STILL producing wrong digests after returning to\n"
               "      %lu MHz / nominal volts - it did not recover on its own.\n",
               (unsigned long)(BASE_KHZ / 1000));
    else if (sha_ph[1].ran && sha_ph[1].bad)
        printf("   ** wrong digests only while boosted; the block recovered after.\n");

    wd_phase = PH_IDLE;
}

/* ================= test 5: how little settle time is survivable ======= */

static const uint32_t settle_list[] = { 4000, 2000, 1000, 600, 400, 250, 150, 100, 60, 30, 10, 0 };
#define N_SETTLE (sizeof(settle_list) / sizeof(settle_list[0]))

static uint32_t chosen_settle_us = 4000;
static uint32_t shortest_good_us = 0xFFFFFFFFu;

static void test_settle_sweep(path_t p) {
    const uint32_t K = 20;

    if (!boosted_exec_ok()) {
        printf("\n== 5. Settle-delay sweep ==\n");
        printf("   skipped: this is the flash build and no working boosted QMI\n");
        printf("   timing was found, so running code at %lu MHz would crash.\n"
               "   Flash the copy_to_ram build instead.\n",
               (unsigned long)(BOOST_KHZ / 1000));
        return;
    }
    printf("\n== 5. Settle-delay sweep via %s ==\n", path_name[p]);
    printf("   Long delays first; stops at the first delay that computes wrong.\n");
    printf("   Each trial checks the core workload, a flash CRC and a SHA-256 digest.\n");
    printf("   settle    enter     exit      verdict\n");
    wd_phase = PH_SWEEP;
    shortest_good_us = 0xFFFFFFFFu;

    for (uint32_t s = 0; s < N_SETTLE; s++) {
        uint32_t d = settle_list[s];
        if (wd_min_bad_settle != 0xFFFFFFFFu && d <= wd_min_bad_settle) {
            printf("   %5lu us   -         -         skipped, hung the board previously\n",
                   (unsigned long)d);
            break;
        }

        uint32_t fails = 0, ent = 0, ext = 0;
        wd_detail = d;
        watchdog_enable(4000, 1);

        for (uint32_t k = 0; k < K; k++) {
            vreg_set_voltage(BASE_VREG_LEVEL);
            busy_wait_ms(RAIL_DECAY_MS);
            seed_buf();
            watchdog_update();

            uint32_t irq = save_and_disable_interrupts();
            uint32_t a = tnow();
            boost_enter(p, d);
            uint32_t b = tnow();
            uint32_t h  = workload(CHECK_ROUNDS);
            uint32_t fc = flash_crc(4096);
            sha256_result_t sd;
            uint32_t su = sha_hw_hash(shabuf, SHA_SWEEP_BYTES, &sd);
            uint32_t c = tnow();
            boost_exit(p);
            uint32_t e = tnow();
            restore_interrupts(irq);

            ent += b - a;
            ext += e - c;
            /* three independent detectors: core ALU, flash interface, SHA block */
            if (h != golden_hash || fc != golden_crc4k ||
                su || !digest_eq(&sd, golden_sha1k.bytes)) fails++;
        }
        watchdog_disable();

        printf("   %5lu us   %5lu us  %5lu us  %s\n",
               (unsigned long)d, (unsigned long)(ent / K), (unsigned long)(ext / K),
               fails ? "WRONG RESULT" : "ok");

        if (fails) { wd_min_bad_settle = d; break; }
        shortest_good_us = d;
    }

    vreg_set_voltage(BASE_VREG_LEVEL);
    busy_wait_ms(RAIL_DECAY_MS);

    if (shortest_good_us != 0xFFFFFFFFu) {
        chosen_settle_us = shortest_good_us * 3;
        if (chosen_settle_us < 50) chosen_settle_us = 50;
        printf("   shortest delay still computing correctly: %lu us\n",
               (unsigned long)shortest_good_us);
        printf("   using %lu us (3x margin) for test 6\n", (unsigned long)chosen_settle_us);
        if (shortest_good_us == 0) {
            printf("   NOTE: 0 us passed, meaning the rail was already high enough\n");
            printf("   before the clock rose. That is margin, not proof that no\n");
            printf("   settling is needed - it will shrink as the die heats up.\n");
        }
    }
    wd_phase = PH_IDLE;
}

/* ================= test 6: round-trip distribution ================= */

static uint32_t rt_mean[2];

static void test_roundtrip(void) {
    const uint32_t K = 100;
    if (!boosted_exec_ok()) { printf("\n== 6. Full round trip ==\n  skipped\n"); return; }
    printf("\n== 6. Full round trip, settle = %lu us ==\n", (unsigned long)chosen_settle_us);
    wd_phase = PH_RT;

    for (int p = 0; p < 2; p++) {
        if (!have_path[p]) { printf("  %-22s skipped\n", path_name[p]); continue; }
        uint32_t sum = 0, mn = 0xFFFFFFFFu, mx = 0;
        wd_detail = (uint32_t)p;
        for (uint32_t k = 0; k < K; k++) {
            vreg_set_voltage(BASE_VREG_LEVEL);
            busy_wait_ms(RAIL_DECAY_MS);
            uint32_t irq = save_and_disable_interrupts();
            uint32_t a = tnow();
            boost_enter((path_t)p, chosen_settle_us);
            boost_exit((path_t)p);
            uint32_t b = tnow();
            restore_interrupts(irq);
            uint32_t dt = b - a;
            sum += dt;
            if (dt < mn) mn = dt;
            if (dt > mx) mx = dt;
        }
        rt_mean[p] = sum / K;
        printf("  %-22s enter+exit  min %lu / mean %lu / max %lu us\n",
               path_name[p], (unsigned long)mn, (unsigned long)rt_mean[p], (unsigned long)mx);
    }
    vreg_set_voltage(BASE_VREG_LEVEL);
    busy_wait_ms(RAIL_DECAY_MS);
    wd_phase = PH_IDLE;
}

/* ================= test 7: throughput and break-even ================= */

static void test_throughput(void) {
    if (!boosted_exec_ok()) { printf("\n== 7. Throughput ==\n  skipped\n"); return; }
    printf("\n== 7. Throughput and break-even ==\n");
    wd_phase = PH_THRU;

    seed_buf();
    uint32_t a = tnow();
    (void)workload(PERF_ROUNDS);
    uint32_t t_base = tnow() - a;

    vreg_set_voltage(BOOST_VREG_LEVEL);
    busy_wait_ms(20);
    if (xip_retimed) set_qmi(boost_qmi_div, boost_qmi_rxd);
    clk_to_boost(PATH_FAST);
    seed_buf();
    a = tnow();
    (void)workload(PERF_ROUNDS);
    uint32_t t_boost = tnow() - a;
    clk_to_base(PATH_FAST);
    if (xip_retimed) set_qmi(base_qmi_div, base_qmi_rxd);
    vreg_set_voltage(BASE_VREG_LEVEL);
    busy_wait_ms(RAIL_DECAY_MS);

    printf("  identical workload: %lu us at %lu MHz, %lu us at %lu MHz\n",
           (unsigned long)t_base,  (unsigned long)(BASE_KHZ / 1000),
           (unsigned long)t_boost, (unsigned long)(BOOST_KHZ / 1000));
    if (!t_boost || t_boost >= t_base) { wd_phase = PH_IDLE; return; }

    uint32_t sx100 = (uint32_t)((uint64_t)t_base * 100u / t_boost);
    printf("  measured speedup %lu.%02lux  (clock ratio %lu.%02lux)\n",
           (unsigned long)(sx100 / 100), (unsigned long)(sx100 % 100),
           (unsigned long)(BOOST_KHZ / BASE_KHZ),
           (unsigned long)((uint64_t)BOOST_KHZ * 100 / BASE_KHZ % 100));

    for (int p = 0; p < 2; p++) {
        if (!have_path[p]) continue;
        uint32_t be = (uint32_t)((uint64_t)rt_mean[p] * sx100 / (sx100 - 100));
        printf("  %-22s pays for itself above %lu us of %lu MHz work\n",
               path_name[p], (unsigned long)be, (unsigned long)(BASE_KHZ / 1000));
    }
    wd_phase = PH_IDLE;
}

/* ================= reporting ================= */

static void report_state(void) {
    printf("  clk_sys %lu kHz  clk_peri %lu kHz  clk_ref %lu kHz\n",
           (unsigned long)(clock_get_hz(clk_sys) / 1000),
           (unsigned long)(clock_get_hz(clk_peri) / 1000),
           (unsigned long)(clock_get_hz(clk_ref) / 1000));
    printf("  VREG VSEL=0x%02lx  VREG_CTRL=0x%08lx  VREG_STS=0x%08lx\n",
           (unsigned long)vreg_get_voltage(),
           (unsigned long)powman_hw->vreg_ctrl, (unsigned long)powman_hw->vreg_sts);
    printf("  PLL_SYS fbdiv=%lu prim=0x%08lx cs=0x%08lx -> VCO %lu MHz\n",
           (unsigned long)(pll_sys_hw->fbdiv_int & PLL_FBDIV_INT_BITS),
           (unsigned long)pll_sys_hw->prim, (unsigned long)pll_sys_hw->cs,
           (unsigned long)((pll_sys_hw->fbdiv_int & PLL_FBDIV_INT_BITS) * (XOSC_HZ / MHZ)));
    printf("  QMI M0_TIMING=0x%08lx\n", (unsigned long)qmi_hw->m[0].timing);
}

static void run_all(void) {
    printf("\n------------------- run start -------------------\n");
    test_xip_timing();      /* first: everything else depends on its result */
    test_primitives();
    test_vout_ok();
    test_sha256();
    test_settle_sweep(PATH_FAST);
    test_roundtrip();
    test_throughput();

    printf("\n== Summary ==\n");
    printf("  voltage register write          : "); pr_ns(ns_vreg_write); printf("\n");
    printf("  QMI retime + dummy read         : "); pr_ns(ns_qmi_write);  printf("\n");
    if (have_path[PATH_SDK]) {
        printf("  150->500 via set_sys_clock_khz  : "); pr_ns(ns_up[0]); printf("\n");
        printf("  500->150 via set_sys_clock_khz  : "); pr_ns(ns_dn[0]); printf("\n");
    }
    printf("  150->500 via postdiv only       : "); pr_ns(ns_up[1]); printf("\n");
    printf("  500->150 via postdiv only       : "); pr_ns(ns_dn[1]); printf("\n");
    if (sha_ph[0].ran) {
        printf("  SHA-256 before / during / after : %lu.%02lu / ",
               (unsigned long)(sha_ph[0].kb_per_s / 1000u),
               (unsigned long)((sha_ph[0].kb_per_s / 10u) % 100u));
        if (sha_ph[1].ran) printf("%lu.%02lu / ",
               (unsigned long)(sha_ph[1].kb_per_s / 1000u),
               (unsigned long)((sha_ph[1].kb_per_s / 10u) % 100u));
        else printf("- / ");
        if (sha_ph[2].ran) printf("%lu.%02lu MB/s\n",
               (unsigned long)(sha_ph[2].kb_per_s / 1000u),
               (unsigned long)((sha_ph[2].kb_per_s / 10u) % 100u));
        else printf("- MB/s\n");
        printf("  SHA-256 digest errors           : %lu / %lu / %lu\n",
               (unsigned long)sha_ph[0].bad, (unsigned long)sha_ph[1].bad,
               (unsigned long)sha_ph[2].bad);
    }
    printf("  settle delay used               : %lu us\n", (unsigned long)chosen_settle_us);
    if (have_path[PATH_SDK])
        printf("  round trip, SDK path            : %lu us\n", (unsigned long)rt_mean[0]);
    printf("  round trip, postdiv path        : %lu us\n", (unsigned long)rt_mean[1]);
    printf("\n  Everything except the settle delay is rounding error. Tune the\n");
    printf("  settle delay on YOUR board with the die warm, then keep margin.\n");
    printf("-------------------- run end --------------------\n");
}

/* ================= main ================= */

int main(void) {
    stdio_init_all();               /* board is at 150 MHz / 1.10 V here */

    gpio_init(MARKER_PIN);
    gpio_set_dir(MARKER_PIN, GPIO_OUT);
    gpio_put(MARKER_PIN, 0);
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 1);

    bool rebooted = watchdog_caused_reboot();
    if (wd_magic != WD_MAGIC) {
        wd_magic = WD_MAGIC;
        wd_min_bad_settle = 0xFFFFFFFFu;
        wd_phase = PH_IDLE;
        wd_detail = 0;
        rebooted = false;
    }
    uint32_t prev_phase = wd_phase, prev_detail = wd_detail;
    wd_phase = PH_IDLE;

    vreg_disable_voltage_limit();   /* sticky until power cycle */

    base_qmi_timing = qmi_hw->m[0].timing;
    base_qmi_div = (base_qmi_timing & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB;
    base_qmi_rxd = (base_qmi_timing & QMI_M0_TIMING_RXDELAY_BITS) >> QMI_M0_TIMING_RXDELAY_LSB;
    if (!base_qmi_div) base_qmi_div = 1;

    for (int i = 0; i < 100 && !stdio_usb_connected(); i++) sleep_ms(100);
    sleep_ms(400);

    printf("\n\n=========================================================\n");
    printf(" boost_bench - RP2350 dynamic clock/voltage overhead\n");
    printf(" %s build | %lu -> %lu MHz | VSEL 0x%02x -> 0x%02x\n",
#if PICO_COPY_TO_RAM
           "copy_to_ram",
#else
           "flash XIP",
#endif
           (unsigned long)(BASE_KHZ / 1000), (unsigned long)(BOOST_KHZ / 1000),
           BASE_VREG_LEVEL, BOOST_VREG_LEVEL);
    printf(" WARNING: drives the core rail far above its rated maximum.\n");
    printf(" GP%d is high while boosted - probe it against the core rail.\n", MARKER_PIN);
    printf("=========================================================\n");
    report_state();

    if (rebooted && prev_phase && prev_phase < PH_MAX) {
        printf("\n  ** the previous run was killed by the watchdog during\n");
        printf("     '%s' (detail=%lu). That setting is unsafe here.\n",
               phase_name[prev_phase], (unsigned long)prev_detail);
    }
    if (wd_min_bad_settle != 0xFFFFFFFFu)
        printf("  ** remembered from an earlier run: settle <= %lu us hangs this board\n",
               (unsigned long)wd_min_bad_settle);

    seed_buf();
    golden_hash  = workload(CHECK_ROUNDS);
    golden_crc1k = flash_crc(1024);
    golden_crc4k = flash_crc(4096);
    printf("\n  golden hash=0x%08lx  crc1k=0x%08lx  crc4k=0x%08lx\n",
           (unsigned long)golden_hash, (unsigned long)golden_crc1k,
           (unsigned long)golden_crc4k);

    sha_fill();
    uint32_t su = sha_hw_hash(shabuf, SHABUF_WORDS * 4u, &golden_sha4k);
    su |= sha_hw_hash(shabuf, SHA_SWEEP_BYTES, &golden_sha1k);
    sha_kat_ok = !su && digest_eq(&golden_sha4k, kat_sha4k)
                     && digest_eq(&golden_sha1k, kat_sha1k);
    printf("  SHA-256 hw digest  = ");
    print_digest(&golden_sha4k);
    printf("\n  known-answer test  : %s\n",
           sha_kat_ok ? "PASS" : "FAIL - hardware or padding disagrees with the reference");
    if (!sha_kat_ok) {
        printf("  expected           = ");
        for (int i = 0; i < 32; i++) printf("%02x", kat_sha4k[i]);
        printf("\n  Later SHA phases still compare against the digest taken here,\n");
        printf("  so before/during/after remains meaningful either way.\n");
    }

    run_all();

    for (;;) {
        printf("\n[r] rerun all  [s] settle sweep  [c] forget crash memory\n> ");
        int ch = getchar_timeout_us(60u * 1000u * 1000u);
        if (ch == 'r' || ch == 'R')      { printf("\n"); run_all(); }
        else if (ch == 's' || ch == 'S') { printf("\n"); test_settle_sweep(PATH_FAST); }
        else if (ch == 'c' || ch == 'C') { wd_min_bad_settle = 0xFFFFFFFFu;
                                           printf("\n  cleared\n"); }
        else if (ch == PICO_ERROR_TIMEOUT) { gpio_xor_mask(1u << PICO_DEFAULT_LED_PIN); }
    }
}
