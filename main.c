/*
 * mulmul_characterize — VR4300 mulmul bug characterization ROM
 *
 * Purpose: enumerate the input/output relationship of the mulmul FP hazard
 * exhaustively enough to derive an emulation rule.
 *
 * Background: back-to-back mul.s instructions produce an incorrect result
 * for the second multiply when the first multiply's operands include sNaN,
 * Zero, or Infinity. A single intervening NOP prevents the corruption.
 * Empirically the corruption appears in the low mantissa bits only.
 *
 * Phases (run sequentially):
 *
 *   Phase 1 — b1 independence check (~instant)
 *     a1=+0, a2=1.5, b2=1.5 (product=2.25, exact in FP)
 *     b1 sampled at PHASE1_SAMPLES evenly-spaced normal values.
 *     Question: does b1's value change the broken result for fixed (a2,b2)?
 *     If all mismatches yield the same broken bits regardless of b1 → b1
 *     is irrelevant to the corruption; only the trigger type matters.
 *
 *   Phase 2 — mantissa sweep per trigger type (~4.5 sec each)
 *     a1 ∈ {+0, −0, +inf, −inf}, b1=1.0, b2=1.0
 *     a2 sweeps all 2^23 mantissa values at exp=127 (1.0 ≤ a2 < 2.0).
 *     Since a2 * 1.0 = a2 exactly in IEEE 754, working = a2.
 *     Any broken != working is directly the corruption as a function of a2.
 *     Question: does trigger type ({+0,−0,+inf,−inf}) change the pattern?
 *
 *   Phase 3 — full positive-normal sweep (~19 min)
 *     a1=+0, b1=1.0, b2=1.0; a2 = all positive normal floats.
 *     Complete characterization for the canonical trigger type.
 *
 * All mismatches logged to debugf as CSV:
 *   phase,a1_bits,b1_bits,a2_bits,b2_bits,broken_bits,working_bits,xor_bits
 *
 * Screen shows phase, progress, and running mismatch count.
 * USB debug output is the primary data channel.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <libdragon.h>

/* -------------------------------------------------------------------------
 * Core probe
 *
 * Both the broken and working sequences are in a single asm block so the
 * compiler cannot insert instructions between the two mul.s instructions
 * in the broken sequence. Operands are loaded explicitly via mtc1 from
 * integer bit patterns to give exact control over every input.
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

/* -------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------- */

static inline bool is_normal(uint32_t bits)
{
    uint32_t exp = (bits >> 23) & 0xFF;
    return (exp != 0) && (exp != 0xFF);
}

/* Log one mismatch line to debugf. All eight fields always present. */
static inline void log_mismatch(const char *phase,
                                uint32_t a1, uint32_t b1,
                                uint32_t a2, uint32_t b2,
                                uint32_t broken, uint32_t working)
{
    debugf("%s,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX\n",
           phase, a1, b1, a2, b2, broken, working, broken ^ working);
}

/* -------------------------------------------------------------------------
 * Phase 1: b1 independence check
 *
 * Fixed: a1=+0, a2=1.5 (0x3FC00000), b2=1.5 (0x3FC00000), product=2.25 exact.
 * Sweep: b1 sampled at PHASE1_SAMPLES evenly-spaced normal bit patterns.
 *
 * Conclusion printed to both screen and debugf:
 *   "b1 matters" if any two mismatches yield different broken bits.
 *   "b1 does not matter" if all mismatches yield the same broken bits.
 * ---------------------------------------------------------------------- */

#define PHASE1_SAMPLES 4096

static void phase1(void)
{
    const uint32_t a1 = 0x00000000;   /* +0   */
    const uint32_t a2 = 0x3FC00000;   /* 1.5  */
    const uint32_t b2 = 0x3FC00000;   /* 1.5, correct product = 2.25 = 0x40100000 */

    uint32_t mismatch_count = 0;
    uint32_t first_broken   = 0;
    bool     first_set      = false;
    bool     b1_matters     = false;

    debugf("# PHASE1 begin: b1 independence check\n");
    debugf("# a1=%08lX a2=%08lX b2=%08lX samples=%lu\n",
           a1, a2, b2, PHASE1_SAMPLES);
    debugf("# cols: phase,a1,b1,a2,b2,broken,working,xor\n");

    console_clear();
    printf("Phase 1: b1 independence check (%lu samples)\n", (uint32_t)PHASE1_SAMPLES);
    console_render();

    /* Step through positive-normal bit space in PHASE1_SAMPLES equal strides. */
    uint64_t step = (uint64_t)(0x7F7FFFFFu - 0x00800000u) / PHASE1_SAMPLES;

    for (uint32_t i = 0; i < PHASE1_SAMPLES; i++) {
        uint32_t b1 = (uint32_t)(0x00800000u + (uint64_t)i * step);
        if (!is_normal(b1)) continue;

        uint32_t broken, working;
        mulmul_probe(a1, b1, a2, b2, &broken, &working);

        if (broken != working) {
            log_mismatch("P1", a1, b1, a2, b2, broken, working);
            mismatch_count++;

            if (!first_set) {
                first_broken = broken;
                first_set    = true;
            } else if (broken != first_broken) {
                b1_matters = true;
            }
        }
    }

    const char *verdict = b1_matters ? "YES — full b1 sweep needed"
                                     : "NO  — b1 value irrelevant";
    debugf("# PHASE1 done: mismatches=%lu b1_matters=%s\n",
           mismatch_count, verdict);
    printf("Phase 1 done: %lu mismatches  b1 matters: %s\n\n",
           mismatch_count, verdict);
    console_render();
}

/* -------------------------------------------------------------------------
 * Phase 2: mantissa sweep per trigger type, exp=127, b2=1.0
 *
 * a2 sweeps all 2^23 mantissa values at exponent 127 (1.0 ≤ a2 < 2.0).
 * b2=1.0 so the IEEE 754 correct result of a2*b2 is a2 exactly.
 * Any broken != working is therefore broken != a2, directly exposing the
 * corruption as a function of a2's bit pattern.
 *
 * Called once per trigger type to test whether {+0,−0,+inf,−inf} produce
 * identical or distinct corruption patterns.
 * ---------------------------------------------------------------------- */

static void phase2_one_trigger(uint32_t a1, uint32_t b1, const char *phase_tag)
{
    const uint32_t b2            = 0x3F800000;  /* 1.0 */
    const uint32_t exp127_base   = 127u << 23;
    const uint32_t mantissa_max  = 1u << 23;
    uint32_t       mismatch_count = 0;

    debugf("# %s begin: a1=%08lX b1=%08lX b2=1.0 exp=127\n",
           phase_tag, a1, b1);

    console_clear();
    printf("%s: mantissa sweep [0..%lu)\n", phase_tag, mantissa_max);
    console_render();

    for (uint32_t mant = 0; mant < mantissa_max; mant++) {
        uint32_t a2 = exp127_base | mant;
        uint32_t broken, working;
        mulmul_probe(a1, b1, a2, b2, &broken, &working);

        if (broken != working) {
            log_mismatch(phase_tag, a1, b1, a2, b2, broken, working);
            mismatch_count++;
        }

        /* Progress update every 64K iterations (~1.5% steps) */
        if ((mant & 0xFFFF) == 0) {
            console_clear();
            printf("%s: %lu / %lu  mismatches: %lu\n",
                   phase_tag, mant, mantissa_max, mismatch_count);
            console_render();
        }
    }

    debugf("# %s done: mismatches=%lu\n", phase_tag, mismatch_count);
    printf("%s done: %lu mismatches\n\n", phase_tag, mismatch_count);
    console_render();
}

static void phase2(void)
{
    /*
     * Four canonical trigger types in the first-pair position.
     * b1=1.0 in all cases (non-trigger, value chosen to be inert).
     */
    static const struct {
        uint32_t    a1;
        const char *tag;
        const char *label;
    } triggers[] = {
        { 0x00000000, "P2_0p",   "+0"   },
        { 0x80000000, "P2_0n",   "-0"   },
        { 0x7F800000, "P2_infp", "+inf" },
        { 0xFF800000, "P2_infn", "-inf" },
    };

    const uint32_t b1 = 0x3F800000;  /* 1.0 */

    for (size_t i = 0; i < sizeof(triggers) / sizeof(triggers[0]); i++) {
        debugf("# Phase 2 trigger: a1=%s\n", triggers[i].label);
        phase2_one_trigger(triggers[i].a1, b1, triggers[i].tag);
    }
}

/* -------------------------------------------------------------------------
 * Phase 3: full positive-normal a2 sweep
 *
 * Same as Phase 2 but a2 covers all positive normal floats (exp 1..254),
 * not just exp=127. Canonical trigger: a1=+0, b1=1.0, b2=1.0.
 * Expected runtime: ~19 minutes on hardware.
 * ---------------------------------------------------------------------- */

static void phase3(void)
{
    const uint32_t a1 = 0x00000000;  /* +0  */
    const uint32_t b1 = 0x3F800000;  /* 1.0 */
    const uint32_t b2 = 0x3F800000;  /* 1.0 */

    uint32_t mismatch_count = 0;

    debugf("# PHASE3 begin: full positive-normal a2 sweep\n");
    debugf("# a1=+0 b1=1.0 b2=1.0 range=[0x00800000..0x7F7FFFFF]\n");

    console_clear();
    printf("Phase 3: full positive-normal sweep\n");
    printf("(~19 minutes on hardware)\n");
    console_render();

    for (uint32_t a2 = 0x00800000u; a2 <= 0x7F7FFFFFu; a2++) {
        /* All values in this range are normal; no is_normal() check needed. */
        uint32_t broken, working;
        mulmul_probe(a1, b1, a2, b2, &broken, &working);

        if (broken != working) {
            log_mismatch("P3", a1, b1, a2, b2, broken, working);
            mismatch_count++;
        }

        /* Progress update every 256K iterations (~3.2% steps) */
        if ((a2 & 0x3FFFF) == 0) {
            console_clear();
            printf("Phase 3: a2=%08lX  mismatches: %lu\n", a2, mismatch_count);
            console_render();
        }
    }

    debugf("# PHASE3 done: mismatches=%lu\n", mismatch_count);
    printf("Phase 3 done: %lu mismatches\n\n", mismatch_count);
    console_render();
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
    console_clear();

    /*
     * Disable FP exceptions that would fire on the trigger operands
     * (invalid operation, divide-by-zero, overflow) for the duration
     * of the sweep.
     */
    uint32_t fcr31_saved = C1_FCR31();
    C1_WRITE_FCR31(fcr31_saved &
        ~(C1_ENABLE_OVERFLOW | C1_ENABLE_DIV_BY_0 | C1_ENABLE_INVALID_OP));

    printf("mulmul characterization ROM\n");
    printf("output → USB debug (CSV)\n\n");
    console_render();

    debugf("# mulmul characterization ROM\n");
    debugf("# cols: phase,a1,b1,a2,b2,broken,working,xor\n");

    phase1();
    phase2();
    phase3();

    C1_WRITE_FCR31(fcr31_saved);

    console_clear();
    printf("All phases complete.\n");
    console_render();

    while (1) {}
}
