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
 * Key design note: b2 must NOT be 1.0 (or any value that makes a2*b2 exact).
 * When a2*b2 is representable without rounding, the FPU rounding stage is
 * a no-op and the corruption does not manifest. Using b2=0x3F8CCCCD (~1.1,
 * not exactly representable) forces rounding for nearly all a2 values,
 * matching the conditions under which ctest.cpp observed hits.
 *
 * Phases (run sequentially):
 *
 *   Sanity — known-triggering input
 *     Tests the specific input from the Buu42 log known to trigger the bug
 *     on affected hardware: (7F800000 * 37BAD25F, 38978B5D * 0C50A394).
 *     Expected broken=05770421, working=05770422 on rev 1.x hardware.
 *     If broken==working here, the asm or hardware is not exhibiting the bug
 *     and subsequent phases will produce no data.
 *
 *   Phase 1 — b1 independence check
 *     a1=+0, a2=0x3D4CCCCD (~0.05), b2=0x3F8CCCCD (~1.1, forces rounding)
 *     b1 sampled at PHASE1_SAMPLES evenly-spaced normal values.
 *     Question: does b1's value change the broken result for fixed (a2,b2)?
 *     If all mismatches yield the same broken bits regardless of b1 → b1
 *     is irrelevant to the corruption; only the trigger type matters.
 *
 *   Phase 2 — mantissa sweep per trigger type
 *     a1 ∈ {+0, −0, +inf, −inf}, b1=0x3F8CCCCD (~1.1)
 *     a2 sweeps all 2^23 mantissa values at exp=117 (small normals).
 *     b2=0x3F8CCCCD (~1.1, forces rounding for all a2 in this band).
 *     Repeated for each trigger type to see if {+0,−0,+inf,−inf} produce
 *     identical or distinct corruption patterns.
 *
 *   Phase 3 — full positive-normal a2 sweep
 *     a1=+0, b1=b2=0x3F8CCCCD (~1.1); a2 = all positive normal floats.
 *     Complete characterization for the canonical trigger type.
 *     Expected runtime: ~19 minutes on hardware.
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

/*
 * b2 used throughout phases 1-3. Must not be 1.0 or any value that makes
 * a2*b2 exactly representable for the a2 ranges under test. 0x3F8CCCCD is
 * the nearest float to 1.1 and is not exactly representable in binary,
 * so it forces rounding for nearly all normal a2 values.
 */
#define B2_FORCING  0x3F8CCCCDu   /* ~1.1, forces rounding */

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
 * Sanity check
 *
 * Tests the exact input from the Buu42 log known to trigger the bug.
 * Prints result to both screen and debug regardless of pass/fail.
 * If broken==working, the bug is absent on this hardware and subsequent
 * phases will produce no meaningful data.
 * ---------------------------------------------------------------------- */

static bool sanity(void)
{
    /* From mulmul-test-log-buu42.txt, last entry — confirmed on rev 1.x */
    const uint32_t a1 = 0x7F800000;  /* +inf */
    const uint32_t b1 = 0x37BAD25F;
    const uint32_t a2 = 0x38978B5D;
    const uint32_t b2 = 0x0C50A394;
    const uint32_t expected_broken  = 0x05770421;
    const uint32_t expected_working = 0x05770422;

    uint32_t broken, working;
    mulmul_probe(a1, b1, a2, b2, &broken, &working);

    bool bug_present  = (broken != working);
    bool result_match = (broken == expected_broken && working == expected_working);

    debugf("# SANITY: a1=%08lX b1=%08lX a2=%08lX b2=%08lX\n", a1, b1, a2, b2);
    debugf("# SANITY: broken=%08lX working=%08lX xor=%08lX\n",
           broken, working, broken ^ working);
    debugf("# SANITY: bug_present=%s result_match=%s\n",
           bug_present  ? "YES" : "NO",
           result_match ? "YES" : "NO");

    console_clear();
    printf("Sanity check (known-triggering input)\n\n");
    printf("  a1=%08lX b1=%08lX\n", a1, b1);
    printf("  a2=%08lX b2=%08lX\n\n", a2, b2);
    printf("  broken  = %08lX  (expect %08lX)\n", broken,  expected_broken);
    printf("  working = %08lX  (expect %08lX)\n", working, expected_working);
    printf("  xor     = %08lX\n\n", broken ^ working);

    if (!bug_present) {
        printf("  RESULT: bug NOT present on this hardware.\n");
        printf("  Subsequent phases will produce no hits.\n");
    } else if (result_match) {
        printf("  RESULT: bug confirmed, values match log.\n");
    } else {
        printf("  RESULT: bug present but values differ from log.\n");
        printf("  (Different hardware revision? Proceed anyway.)\n");
    }

    console_render();
    return bug_present;
}

/* -------------------------------------------------------------------------
 * Phase 1: b1 independence check
 *
 * Fixed: a1=+0, a2=0x3D4CCCCD (~0.05), b2=B2_FORCING (~1.1).
 * Product is not exactly representable, so rounding occurs and the
 * corruption has a chance to manifest.
 * Sweep: b1 sampled at PHASE1_SAMPLES evenly-spaced normal bit patterns.
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
    debugf("# a1=%08lX a2=%08lX b2=%08lX samples=%lu\n",
           a1, a2, b2, (uint32_t)PHASE1_SAMPLES);

    console_clear();
    printf("Phase 1: b1 independence check (%lu samples)\n",
           (uint32_t)PHASE1_SAMPLES);
    console_render();

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
 * Phase 2: mantissa sweep per trigger type
 *
 * a2 sweeps all 2^23 mantissa values at exp=117 (small normals,
 * matching the range of second-pair products seen in the Buu42 log).
 * b2=B2_FORCING (~1.1) forces rounding for all a2 in this band.
 * ---------------------------------------------------------------------- */

static void phase2_one_trigger(uint32_t a1, uint32_t b1, const char *phase_tag)
{
    const uint32_t b2           = B2_FORCING;
    const uint32_t exp117_base  = 117u << 23;
    const uint32_t mantissa_max = 1u << 23;
    uint32_t       mismatch_count = 0;

    debugf("# %s begin: a1=%08lX b1=%08lX b2=%08lX exp=117\n",
           phase_tag, a1, b1, b2);

    console_clear();
    printf("%s: mantissa sweep exp=117 [0..%lu)\n", phase_tag, mantissa_max);
    console_render();

    for (uint32_t mant = 0; mant < mantissa_max; mant++) {
        uint32_t a2 = exp117_base | mant;
        uint32_t broken, working;
        mulmul_probe(a1, b1, a2, b2, &broken, &working);

        if (broken != working) {
            log_mismatch(phase_tag, a1, b1, a2, b2, broken, working);
            mismatch_count++;
        }

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

    const uint32_t b1 = B2_FORCING;  /* ~1.1, non-trigger normal */

    for (size_t i = 0; i < sizeof(triggers) / sizeof(triggers[0]); i++) {
        debugf("# Phase 2 trigger: a1=%s\n", triggers[i].label);
        phase2_one_trigger(triggers[i].a1, b1, triggers[i].tag);
    }
}

/* -------------------------------------------------------------------------
 * Phase 3: full positive-normal a2 sweep
 *
 * a1=+0, b1=b2=B2_FORCING; a2 = all positive normal floats.
 * Expected runtime: ~19 minutes on hardware.
 * ---------------------------------------------------------------------- */

static void phase3(void)
{
    const uint32_t a1 = 0x00000000;  /* +0   */
    const uint32_t b1 = B2_FORCING;  /* ~1.1 */
    const uint32_t b2 = B2_FORCING;  /* ~1.1 */

    uint32_t mismatch_count = 0;

    debugf("# PHASE3 begin: full positive-normal a2 sweep\n");
    debugf("# a1=+0 b1=%08lX b2=%08lX range=[0x00800000..0x7F7FFFFF]\n",
           b1, b2);

    console_clear();
    printf("Phase 3: full positive-normal sweep (~19 min)\n");
    console_render();

    for (uint32_t a2 = 0x00800000u; a2 <= 0x7F7FFFFFu; a2++) {
        uint32_t broken, working;
        mulmul_probe(a1, b1, a2, b2, &broken, &working);

        if (broken != working) {
            log_mismatch("P3", a1, b1, a2, b2, broken, working);
            mismatch_count++;
        }

        if ((a2 & 0x3FFFFu) == 0) {
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

    uint32_t fcr31_saved = C1_FCR31();
    C1_WRITE_FCR31(fcr31_saved &
        ~(C1_ENABLE_OVERFLOW | C1_ENABLE_DIV_BY_0 | C1_ENABLE_INVALID_OP));

    printf("mulmul characterization ROM\n");
    printf("output -> USB debug (CSV)\n\n");
    console_render();

    debugf("# mulmul characterization ROM\n");
    debugf("# cols: phase,a1,b1,a2,b2,broken,working,xor\n");

    bool bug_present = sanity();

    if (bug_present) {
        phase1();
        phase2();
        phase3();
    } else {
        console_clear();
        printf("Bug not present on this hardware.\n");
        printf("Phases 1-3 skipped.\n");
        console_render();
    }

    C1_WRITE_FCR31(fcr31_saved);

    console_clear();
    printf(bug_present ? "All phases complete.\n" : "Done (no bug).\n");
    console_render();

    while (1) {}
}
