/*
 * mulmul_characterize
 *
 * Sweep A: b1 characterization
 *   Fix a1=+0, a2=SWEEP_A_A2, b2=B2_FORCING.
 *   Sweep b1 over every positive normal float (0x00800000..0x7F7FFFFF).
 *   Goal: map b1_mant -> XOR to fully characterize b1's influence.
 *
 * Sweep B: a2 mantissa characterization
 *   Fix a1=+0, b1=B2_FORCING, b2=B2_FORCING.
 *   Sweep a2 mantissa 0x000000..0x7FFFFF step 1 at exp=117.
 *   Goal: verify and densify the 5-step periodic XOR formula discovered
 *   in Phase 2 (which sampled only step-512 and selected offsets).
 *
 * CSV cols: phase,a1,b1,a2,b2,broken,working,xor
 */

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <libdragon.h>

/* ~1.1: forces rounding in the second mul, triggering the hazard */
#define B2_FORCING  0x3F8CCCCDu

/*
 * Sweep A anchor: exp=117, mant=0x000C00.
 * This value produced XOR=0x3FA in Phase 2: a large, clean signal.
 * Same (a2, b2) pair as Phase 1's a2 trigger (0x3D4CCCCD) would also
 * work; 0x3A800C00 is preferred because its XOR is larger and easier
 * to distinguish in the b1 sweep.
 */
#define SWEEP_A_A2          0x3A800C00u

/* exp=117, matching Phase 2 */
#define SWEEP_B_EXP_BASE    (117u << 23)

#define SWEEP_A_LOG_LIMIT   500000u
#define SWEEP_B_LOG_LIMIT   500000u

static uint32_t a_found  = 0;
static uint32_t a_logged = 0;
static uint32_t b_found  = 0;
static uint32_t b_logged = 0;

/* -------------------------------------------------------------------------
 * Core probe — unchanged from prior ROM
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
 * Sanity check (from Buu42 logs of HailtoDodongo's mulmul test ROM)
 * ---------------------------------------------------------------------- */
static bool sanity(void)
{
    const uint32_t a1 = 0x7F800000u;
    const uint32_t b1 = 0x37BAD25Fu;
    const uint32_t a2 = 0x38978B5Du;
    const uint32_t b2 = 0x0C50A394u;

    uint32_t broken, working;
    mulmul_probe(a1, b1, a2, b2, &broken, &working);

    debugf("# SANITY: broken=%08lX working=%08lX xor=%08lX\n",
           broken, working, broken ^ working);

    console_clear();
    printf("Sanity check\n\n");
    printf("  broken  = %08lX\n  working = %08lX\n\n", broken, working);
    console_render();

    return (broken != working);
}

/* -------------------------------------------------------------------------
 * Sweep A: b1 sweep
 *
 * b1 ranges over all positive normals: exponent 1..254, any mantissa.
 * That is exactly 0x00800000..0x7F7FFFFF.
 * ~8.4 M iterations.
 * ---------------------------------------------------------------------- */
static void sweep_a(void)
{
    const uint32_t a1 = 0x00000000u;
    const uint32_t a2 = SWEEP_A_A2;
    const uint32_t b2 = B2_FORCING;

    debugf("# SWEEP_A begin  a2=%08lX b2=%08lX\n", a2, b2);

    for (uint32_t b1 = 0x00800000u; b1 <= 0x7F7FFFFFu; b1++) {

        uint32_t broken, working;
        mulmul_probe(a1, b1, a2, b2, &broken, &working);

        if (broken != working) {
            a_found++;
            if (a_logged < SWEEP_A_LOG_LIMIT) {
                a_logged++;
                debugf("SA,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX\n",
                       a1, b1, a2, b2, broken, working, broken ^ working);
            }
        }

        /* UI update roughly every 1M iterations */
        if ((b1 & 0xFFFFFu) == 0x80000u) {
            console_clear();
            printf("Sweep A: b1 sweep\n\n");
            printf("  b1      = %08lX\n", b1);
            printf("  found   = %lu\n", a_found);
            printf("  logged  = %lu / %lu\n", a_logged, (uint32_t)SWEEP_A_LOG_LIMIT);
            console_render();
        }
    }

    debugf("# SWEEP_A done  found=%lu logged=%lu\n", a_found, a_logged);
}

/* -------------------------------------------------------------------------
 * Sweep B: a2 mantissa sweep (step 1)
 *
 * Phase 2 sampled every 512th mantissa value plus selected bit-pattern
 * offsets. This sweep fills in every mantissa at the same exponent (117)
 * to verify the 5-step periodic formula and catch any non-sampled behavior.
 * ~8.4 M iterations.
 * ---------------------------------------------------------------------- */
static void sweep_b(void)
{
    const uint32_t a1 = 0x00000000u;
    const uint32_t b1 = B2_FORCING;
    const uint32_t b2 = B2_FORCING;

    debugf("# SWEEP_B begin  b1=%08lX b2=%08lX exp_base=%08lX\n",
           b1, b2, (uint32_t)SWEEP_B_EXP_BASE);

    for (uint32_t mant = 0x000000u; mant <= 0x7FFFFFu; mant++) {

        uint32_t a2 = SWEEP_B_EXP_BASE | mant;
        uint32_t broken, working;
        mulmul_probe(a1, b1, a2, b2, &broken, &working);

        if (broken != working) {
            b_found++;
            if (b_logged < SWEEP_B_LOG_LIMIT) {
                b_logged++;
                debugf("SB,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX\n",
                       a1, b1, a2, b2, broken, working, broken ^ working);
            }
        }

        /* UI update roughly every 512K iterations */
        if ((mant & 0x7FFFFu) == 0) {
            console_clear();
            printf("Sweep B: a2 mant sweep\n\n");
            printf("  mant    = %06lX / 7FFFFF\n", mant);
            printf("  found   = %lu\n", b_found);
            printf("  logged  = %lu / %lu\n", b_logged, (uint32_t)SWEEP_B_LOG_LIMIT);
            console_render();
        }
    }

    debugf("# SWEEP_B done  found=%lu logged=%lu\n", b_found, b_logged);
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

    C1_WRITE_FCR31(C1_FCR31() &
                   ~(C1_ENABLE_OVERFLOW | C1_ENABLE_DIV_BY_0 | C1_ENABLE_INVALID_OP));

    console_clear();
    printf("mulmul targeted sweeps\n");
    console_render();

    debugf("# mulmul targeted sweeps\n");
    debugf("# cols: phase,a1,b1,a2,b2,broken,working,xor\n");
    debugf("# SA log cap: %lu   SB log cap: %lu\n",
           (uint32_t)SWEEP_A_LOG_LIMIT, (uint32_t)SWEEP_B_LOG_LIMIT);

    if (!sanity()) {
        console_clear();
        printf("SANITY FAILED\nbug not present on this unit.\n");
        console_render();
        while (1) {}
    }

    sweep_a();
    sweep_b();

    console_clear();
    printf("Done.\n\n");
    printf("Sweep A  found=%lu  logged=%lu\n", a_found, a_logged);
    printf("Sweep B  found=%lu  logged=%lu\n", b_found, b_logged);
    console_render();

    while (1) {}
}
