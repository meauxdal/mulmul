/*
 * mulmul_characterize — VR4300 mulmul bug characterization ROM
 *
 * Purpose: Enumerate the input/output relationship of the mulmul FP hazard
 * on targeted rounding boundaries to stay under a 5-minute hardware testing window.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <libdragon.h>

#define B2_FORCING        0x3F8CCCCDu   /* ~1.1, forces rounding */
#define PHASE1_LOG_LIMIT  100u
#define PHASE2_LOG_LIMIT  90000u
#define PHASE3_LOG_LIMIT  90000u
#define MAX_LOGGED_MISMATCHES (PHASE1_LOG_LIMIT + PHASE2_LOG_LIMIT + PHASE3_LOG_LIMIT)

static uint32_t total_discovered = 0;
static uint32_t total_logged = 0;
static uint32_t logged_phase1 = 0;
static uint32_t logged_phase2 = 0;
static uint32_t logged_phase3 = 0;

/* -------------------------------------------------------------------------
 * Core probe
 * ---------------------------------------------------------------------- */
static void mulmul_probe(uint32_t a1, uint32_t b1,
                         uint32_t a2, uint32_t b2,
                         uint32_t *broken_out, uint32_t *working_out)
{
    uint32_t broken, working;
    __asm__ volatile (
        "mtc1   %2, $f12\n"
        "mtc1   %3, $f13\n"
        "mtc1   %4, $f14\n"
        "mtc1   %5, $f15\n"
        /* broken: back-to-back mul.s */
        "mul.s  $f0, $f12, $f13\n"
        "mul.s  $f1, $f14, $f15\n"
        "mfc1   %0, $f1\n"
        /* working: NOP between mul.s */
        "mul.s  $f0, $f12, $f13\n"
        "nop\n"
        "mul.s  $f1, $f14, $f15\n"
        "mfc1   %1, $f1\n"
        : "=r"(broken), "=r"(working)
        : "r"(a1), "r"(b1), "r"(a2), "r"(b2)
        : "$f0", "$f1", "$f12", "$f13", "$f14", "$f15"
    );
    *broken_out  = broken;
    *working_out = working;
}

static inline bool is_normal(uint32_t bits)
{
    uint32_t exp = (bits >> 23) & 0xFF;
    return (exp != 0) && (exp != 0xFF);
}

static inline void log_mismatch(const char *phase,
                                uint32_t a1, uint32_t b1,
                                uint32_t a2, uint32_t b2,
                                uint32_t broken, uint32_t working,
                                uint32_t *phase_logged, uint32_t phase_limit)
{
    total_discovered++;
    if (*phase_logged >= phase_limit) return;
    (*phase_logged)++;
    total_logged++;

    debugf("%s,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX\n",
           phase, a1, b1, a2, b2, broken, working, broken ^ working);
}

/* -------------------------------------------------------------------------
 * Sanity check
 * ---------------------------------------------------------------------- */
static bool sanity(void)
{
    const uint32_t a1 = 0x7F800000;  /* +inf */
    const uint32_t b1 = 0x37BAD25F;
    const uint32_t a2 = 0x38978B5D;
    const uint32_t b2 = 0x0C50A394;

    uint32_t broken, working;
    mulmul_probe(a1, b1, a2, b2, &broken, &working);

    bool bug_present  = (broken != working);

    debugf("# SANITY: a1=%08lX b1=%08lX a2=%08lX b2=%08lX\n", a1, b1, a2, b2);
    debugf("# SANITY: broken=%08lX working=%08lX xor=%08lX\n", broken, working, broken ^ working);

    console_clear();
    printf("Sanity check\n\n");
    printf("  broken  = %08lX\n  working = %08lX\n\n", broken, working);
    console_render();

    return bug_present;
}

/* -------------------------------------------------------------------------
 * Phase 1: b1 independence check (4096 samples ~5 seconds)
 * ---------------------------------------------------------------------- */
#define PHASE1_SAMPLES 4096u

static void phase1(void)
{
    const uint32_t a1 = 0x00000000;   /* +0            */
    const uint32_t a2 = 0x3D4CCCCDu;  /* ~0.05         */
    const uint32_t b2 = B2_FORCING;   /* ~1.1          */

    uint32_t mismatch_count = 0;
    uint32_t first_broken   = 0;
    bool     first_set      = false;
    bool     b1_matters     = false;

    debugf("# PHASE1 begin: b1 independence check\n");

    uint64_t step = (uint64_t)(0x7F7FFFFFu - 0x00800000u) / PHASE1_SAMPLES;

    for (uint32_t i = 0; i < PHASE1_SAMPLES; i++) {
        uint32_t b1 = (uint32_t)(0x00800000u + (uint64_t)i * step);
        if (!is_normal(b1)) continue;

        uint32_t broken, working;
        mulmul_probe(a1, b1, a2, b2, &broken, &working);

        if (broken != working) {
            log_mismatch("P1", a1, b1, a2, b2, broken, working,
                         &logged_phase1, PHASE1_LOG_LIMIT);
            mismatch_count++;

            if (!first_set) {
                first_broken = broken;
                first_set    = true;
            } else if (broken != first_broken) {
                b1_matters = true;
            }
        }
    }

    const char *verdict = b1_matters ? "YES" : "NO";
    debugf("# PHASE1 done: mismatches=%lu b1_matters=%s\n", mismatch_count, verdict);
}

/* -------------------------------------------------------------------------
 * Phase 2: Targeted carry-chain mantissa sweep
 * ---------------------------------------------------------------------- */
static void phase2_one_trigger(uint32_t a1, uint32_t b1, const char *phase_tag)
{
    const uint32_t b2           = B2_FORCING;
    const uint32_t exp117_base  = 117u << 23;
    uint32_t       mismatch_count = 0;

    debugf("# %s begin: Targeted sweep\n", phase_tag);

    for (uint32_t base_mant = 0; base_mant < (1u << 23); base_mant += 512) {
        uint32_t test_mantissas[] = {
            base_mant,
            base_mant | 0x1F,
            base_mant | 0x3F,
            base_mant | 0x7F,
            base_mant | 0xFF
        };

        for(int m = 0; m < 5; m++) {
            uint32_t mant = test_mantissas[m];
            if (mant >= (1u << 23)) continue;

            uint32_t a2 = exp117_base | mant;
            uint32_t broken, working;
            mulmul_probe(a1, b1, a2, b2, &broken, &working);

            if (broken != working) {
                log_mismatch(phase_tag, a1, b1, a2, b2, broken, working,
                             &logged_phase2, PHASE2_LOG_LIMIT);
                mismatch_count++;
            }
        }

        // Lightweight UI progress tick (~every 64 outer loops)
        if ((base_mant & 0x7FFF) == 0) {
            console_clear();
            printf("Running Phase 2 (%s)...\n", phase_tag);
            printf("Total mismatches found: %lu\n", total_discovered);
            printf("USB logs written:      %lu / %u\n", total_logged, MAX_LOGGED_MISMATCHES);
            console_render();
        }
    }
    debugf("# %s done: mismatches=%lu\n", phase_tag, mismatch_count);
}

static void phase2(void)
{
    static const struct {
        uint32_t    a1;
        const char *tag;
    } triggers[] = {
        { 0x00000000, "P2_0p"   },
        { 0x80000000, "P2_0n"   },
        { 0x7F800000, "P2_infp" },
        { 0xFF800000, "P2_infn" },
    };

    const uint32_t b1 = B2_FORCING;
    for (size_t i = 0; i < sizeof(triggers) / sizeof(triggers[0]); i++) {
        phase2_one_trigger(triggers[i].a1, b1, triggers[i].tag);
    }
}

/* -------------------------------------------------------------------------
 * Phase 3: Exponent Sweep + Precision Carry-Chain testing
 * ---------------------------------------------------------------------- */
static void phase3(void)
{
    const uint32_t a1 = 0x00000000; 
    const uint32_t b1 = B2_FORCING; 
    const uint32_t b2 = B2_FORCING; 

    uint32_t mismatch_count = 0;
    debugf("# PHASE3 begin: Targeted Exponent & Rounding Sweep\n");

    for (uint32_t exp = 1; exp < 255; exp++) {
        uint32_t exp_base = exp << 23;

        for (uint32_t base_mant = 0; base_mant < (1u << 23); base_mant += 4096) {
            uint32_t edge_cases[] = {
                base_mant,
                base_mant | 0x0F,
                base_mant | 0x1F,
                base_mant | 0x3F,
                base_mant | 0x7F,
                base_mant | 0xFF,
                base_mant | 0x1FF,
                base_mant | 0x3FF
            };

            for (int e = 0; e < 8; e++) {
                uint32_t mant = edge_cases[e];
                if (mant >= (1u << 23)) continue;

                uint32_t a2 = exp_base | mant;
                uint32_t broken, working;
                mulmul_probe(a1, b1, a2, b2, &broken, &working);

                if (broken != working) {
                    log_mismatch("P3", a1, b1, a2, b2, broken, working,
                                 &logged_phase3, PHASE3_LOG_LIMIT);
                    mismatch_count++;
                }
            }
        }

        // Visual reassurance for the tester every 8 exponents
        if ((exp & 0x07) == 0) {
            console_clear();
            printf("Running Phase 3 (Exp %lu/254)...\n", exp);
            printf("Total mismatches found: %lu\n", total_discovered);
            printf("USB logs written:      %lu / %u\n", total_logged, MAX_LOGGED_MISMATCHES);
            console_render();
        }
    }
    debugf("# PHASE3 done: mismatches=%lu\n", mismatch_count);
}

/* -------------------------------------------------------------------------
 * Entry point
 * ---------------------------------------------------------------------- */
int main(void)
{
    debug_init_isviewer();
    debug_init_usblog();

    console_init();
    console_set_render_mode(RENDER_MANUAL);
    
    C1_WRITE_FCR31(C1_FCR31() & ~(C1_ENABLE_OVERFLOW | C1_ENABLE_DIV_BY_0 | C1_ENABLE_INVALID_OP));

    console_clear();
    printf("mulmul characterization ROM (Fast-Targeted)\n");
    console_render();

    debugf("# mulmul characterization ROM (Fast-Targeted)\n");
    debugf("# cols: phase,a1,b1,a2,b2,broken,working,xor\n");
    debugf("# logging first %u mismatches only; later mismatches are counted but not detailed\n", MAX_LOGGED_MISMATCHES);
    debugf("# phase budget: P1=%u, P2=%u, P3=%u\n", PHASE1_LOG_LIMIT, PHASE2_LOG_LIMIT, PHASE3_LOG_LIMIT);

    (void)sanity();

    phase1();
    phase2();
    phase3();

    console_clear();
    printf("All phases complete.\n");
    printf("Total mismatches found: %lu\n", total_discovered);
    printf("Total logs generated:   %lu\n", total_logged);
    console_render();

    while (1) {}
}