#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <libdragon.h>

#define B2_FORCING   0x3F8CCCCDu
#define SWEEP_A_A2   0x3A800C00u

/* Cap strictly at ~2 mins of USB transfer (assuming 800 logs/sec) */
#define LOG_LIMIT    100000u 

static inline void mulmul_probe(uint32_t a1, uint32_t b1, 
                                uint32_t a2, uint32_t b2, 
                                uint32_t *broken_out, uint32_t *working_out) 
{
    uint32_t broken, working;
    __asm__ volatile (
        "mtc1   %2, $f12\n"
        "mtc1   %3, $f13\n"
        "mtc1   %4, $f14\n"
        "mtc1   %5, $f15\n"
        /* broken: back-to-back */
        "mul.s  $f0, $f12, $f13\n"
        "mul.s  $f1, $f14, $f15\n"
        "mfc1   %0, $f1\n"
        /* working: pipeline flushed with nop */
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

    /* Suppress exceptions that might crash the sweep */
    C1_WRITE_FCR31(C1_FCR31() & ~(C1_ENABLE_OVERFLOW | C1_ENABLE_DIV_BY_0 | C1_ENABLE_INVALID_OP));

    console_clear();
    printf("Theory #1: Accumulator Leakage Test\nRunning...\n");
    console_render();

    debugf("# Leakage test (a1 != 0)\n");
    debugf("# cols: b1,broken,working,xor,residue\n");

    /* High entropy a1 to ensure a complex/dirty multiplier tree */
    const uint32_t a1 = 0x3F9E0651u; 
    const uint32_t a2 = SWEEP_A_A2;
    const uint32_t b2 = B2_FORCING;

    /* Pre-convert a1 to double for the software exact-math check */
    float fa1;
    memcpy(&fa1, &a1, 4);
    double da1 = (double)fa1;

    uint32_t logged = 0;

    /* Sweep b1 across all normal values between 1.0 and 2.0 (~8.3M iterations) */
    for (uint32_t b1 = 0x3F800000u; b1 <= 0x3FFFFFFFu; b1++) {
        uint32_t broken, working;
        mulmul_probe(a1, b1, a2, b2, &broken, &working);

        if (broken != working) {
            if (logged < LOG_LIMIT) {
                logged++;

                /* Calculate mathematical exact product to find discarded bits */
                float fb1;
                memcpy(&fb1, &b1, 4);
                
                union { double d; uint64_t u; } exact_val;
                exact_val.d = da1 * (double)fb1;
                
                /* * A double mantissa is 52 bits. A single mantissa is 23 bits.
                 * Single precision keeps the top 23 bits of this exact product.
                 * The remaining lower 29 bits contain the "residue" (Guard, Round, 
                 * Sticky, and discarded bits) that were active in the ALU.
                 */
                uint32_t residue = (uint32_t)(exact_val.u & 0x1FFFFFFFllu);

                debugf("%08lX,%08lX,%08lX,%08lX,%08lX\n", b1, broken, working, broken ^ working, residue);
            } else {
                /* Hard abort to respect the 5-minute runtime constraint */
                break; 
            }
        }
    }

    console_clear();
    printf("Done.\nLogged: %lu\n", logged);
    console_render();
    debugf("# DONE\n");

    while (1) {}
}
