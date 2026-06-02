#include <stdio.h>
#include <stdint.h>
#include <libdragon.h>

/* -----------------------------------------------------------------------
 * Sanity vector — known to trigger on affected hardware.
 * Source: prior hardware run (buu42).
 * ----------------------------------------------------------------------- */
#define SANITY_A1          0x7F800000u
#define SANITY_B1          0x37BAD25Fu
#define SANITY_A2          0x38978B5Du
#define SANITY_B2          0x0C50A394u
#define SANITY_EXP_BROKEN  0x05770421u
#define SANITY_EXP_WORKING 0x05770422u

/* -----------------------------------------------------------------------
 * Sweep — Phase-1 parameters, previously confirmed to produce mismatches.
 *
 * Hypothesis under test: b1 feeds state through the FPU B-operand path
 * even when a1=0, so XOR should scale predictably with b1's mantissa.
 *
 * a1 = 0 (zero)         mul1 product is always exactly 0;
 *                        any b1-dependent XOR must come from
 *                        b1 state in the multiplier, not the result.
 * a2 = 0x3D4CCCCD       fixed second-multiply operand
 * b2 = 0x3F8CCCCD (~1.1) forces rounding in mul2
 * b1 sweeps from smallest positive normal upward
 *
 * Tune LOG_LIMIT to control output size/runtime.
 * ~800 lines logged per second via USB (bottleneck)
 * ----------------------------------------------------------------------- */
#define SWEEP_A1        0x00000000u
#define SWEEP_A2        0x3D4CCCCDu
#define SWEEP_B2        0x3F8CCCCDu
#define SWEEP_B1_START  0x00800000u   /* smallest positive normal */
#define SWEEP_B1_END    0x3FFFFFFFu   /* LOG_LIMIT will abridge this */

#define LOG_LIMIT       10000u
#define CONSOLE_EVERY   500000u       /* redraw console every N iterations */

static inline void mulmul_probe(uint32_t a1, 
                                uint32_t b1,
                                uint32_t a2, 
                                uint32_t b2,
                                uint32_t *broken_out, 
                                uint32_t *working_out)
{
    uint32_t broken, working;
    __asm__ volatile (
        "mtc1   %2, $f12\n"
        "mtc1   %3, $f13\n"
        "mtc1   %4, $f14\n"
        "mtc1   %5, $f15\n"
        /* broken: back-to-back muls */
        "mul.s  $f0, $f12, $f13\n"
        "mul.s  $f1, $f14, $f15\n"
        "mfc1   %0, $f1\n"
        /* working: nop between muls */
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

int main(void)
{
    debug_init_isviewer();
    debug_init_usblog();
    console_init();
    console_set_render_mode(RENDER_MANUAL);

    C1_WRITE_FCR31(C1_FCR31() &
        ~(C1_ENABLE_OVERFLOW | C1_ENABLE_DIV_BY_0 | C1_ENABLE_INVALID_OP));

    /* -------------------------------------------------------------------
     * Sanity check — NON-GATING.
     * Prints verdict to log and screen; sweep runs regardless of result.
     * ------------------------------------------------------------------- */
    const char *sanity_verdict;
    {
        uint32_t broken, working;
        mulmul_probe(SANITY_A1, SANITY_B1, SANITY_A2, SANITY_B2,
                     &broken, &working);

        if (broken == SANITY_EXP_BROKEN && working == SANITY_EXP_WORKING)
            sanity_verdict = "PASS";
        else if (broken == working)
            sanity_verdict = "NO-BUG";      /* unit unaffected, or probe broken */
        else
            sanity_verdict = "UNEXPECTED";  /* triggered but wrong values */

        debugf("# SANITY %s broken=%08lX working=%08lX xor=%08lX\n",
               sanity_verdict,
               (unsigned long)broken,
               (unsigned long)working,
               (unsigned long)(broken ^ working));
    }

    console_clear();
    printf("Sanity: %s\n\nSweeping...\n", sanity_verdict);
    console_render();

    /* -------------------------------------------------------------------
     * Main sweep.
     *
     * CSV output columns:
     *   b1         — sweep input (hex)
     *   xor        — broken XOR working (hex): which bits differ
     *   delta_ulps — |broken - working| (decimal): magnitude of error
     *
     * For positive normals in the same binade, delta_ulps equals the
     * raw integer difference between the two bit patterns, which is
     * the number of representable values between them (ULP count).
     * xor and delta_ulps are related but not equal for multi-bit errors.
     * ------------------------------------------------------------------- */
    debugf("# SWEEP a1=%08lX a2=%08lX b2=%08lX limit=%lu\n",
           (unsigned long)SWEEP_A1,
           (unsigned long)SWEEP_A2,
           (unsigned long)SWEEP_B2,
           (unsigned long)LOG_LIMIT);
    debugf("# b1,xor,delta_ulps\n");

    uint32_t logged = 0;
    uint32_t b1;

    for (b1 = SWEEP_B1_START; b1 <= SWEEP_B1_END; b1++) {

        /* Periodic console update so the screen is not blank */
        if ((b1 - SWEEP_B1_START) % CONSOLE_EVERY == 0) {
            console_clear();
            printf("Sanity: %s\n\nb1:     %08lX\nLogged: %lu / %lu\n",
                   sanity_verdict,
                   (unsigned long)b1,
                   (unsigned long)logged,
                   (unsigned long)LOG_LIMIT);
            console_render();
        }

        uint32_t broken, working;
        mulmul_probe(SWEEP_A1, b1, SWEEP_A2, SWEEP_B2, &broken, &working);

        if (broken != working) {
            uint32_t delta = (broken > working)
                             ? (broken - working)
                             : (working - broken);

            debugf("%08lX,%08lX,%lu\n",
                   (unsigned long)b1,
                   (unsigned long)(broken ^ working),
                   (unsigned long)delta);

            if (++logged >= LOG_LIMIT)
                break;
        }
    }

    debugf("# DONE logged=%lu last_b1=%08lX\n",
           (unsigned long)logged,
           (unsigned long)b1);

    console_clear();
    printf("Sanity: %s\n\nDone.\nLogged: %lu\n",
           sanity_verdict, (unsigned long)logged);
    console_render();

    while (1) {}
}
