/*
 * mulmul_timing — CP0 Count throughput calibration
 *
 * Measures:
 *   A) mulmul_probe() with no USB logging       (compute ceiling)
 *   B) mulmul_probe() + unconditional debugf()  (logging floor)
 *
 * The log bench is unconditional so it gives accurate USB overhead
 * regardless of hardware revision (no bug = broken==working, but we
 * log anyway).
 *
 * Prints a runtime estimate table for the iteration counts we plan
 * to use in the characterization ROM phases.
 *
 * CP0 Count: increments at CPU_FREQ/2 = 46,875,000 Hz.
 * Wraps at ~91.6 s. Keep N_LOG small enough to stay well under this.
 */

#include <stdio.h>
#include <stdint.h>
#include <libdragon.h>

/* -------------------------------------------------------------------------
 * CP0 Count
 * ---------------------------------------------------------------------- */

#define COUNT_HZ  46875000u

static inline uint32_t count_read(void)
{
    uint32_t c;
    __asm__ volatile ("mfc0 %0, $9" : "=r"(c));
    return c;
}

/* -------------------------------------------------------------------------
 * Probe (identical to characterization ROM)
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
        "mul.s  $f0, $f12, $f13\n"
        "mul.s  $f1, $f14, $f15\n"
        "mfc1   %0, $f1\n"
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

/* Fixed inputs from Buu42 log; known to trigger the bug on rev 1.x.
 * Used here purely as stable probe inputs — bug presence irrelevant. */
#define BM_A1  0x7F800000u
#define BM_B1  0x37BAD25Fu
#define BM_A2  0x38978B5Du
#define BM_B2  0x0C50A394u

/* -------------------------------------------------------------------------
 * Benchmarks
 * ---------------------------------------------------------------------- */

#define N_COMPUTE  1000000u   /* 1M iters, no logging   */
#define N_LOG        1000u    /* 1K iters, all logged   */

static uint32_t bench_compute(void)
{
    uint32_t broken, working;
    uint32_t t0 = count_read();
    for (uint32_t i = 0; i < N_COMPUTE; i++)
        mulmul_probe(BM_A1, BM_B1, BM_A2, BM_B2, &broken, &working);
    return count_read() - t0;
}

static uint32_t bench_log(void)
{
    uint32_t broken, working;
    uint32_t t0 = count_read();
    for (uint32_t i = 0; i < N_LOG; i++) {
        mulmul_probe(BM_A1, BM_B1, BM_A2, BM_B2, &broken, &working);
        debugf("T,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX,%08lX\n",
               (unsigned long)BM_A1, (unsigned long)BM_B1,
               (unsigned long)BM_A2, (unsigned long)BM_B2,
               (unsigned long)broken, (unsigned long)working,
               (unsigned long)(broken ^ working));
    }
    return count_read() - t0;
}

/* -------------------------------------------------------------------------
 * Display helpers
 * ---------------------------------------------------------------------- */

/* iter/s from tick count */
static uint32_t rate_of(uint32_t n, uint32_t ticks)
{
    if (ticks == 0) return 0;
    return (uint32_t)((uint64_t)n * COUNT_HZ / ticks);
}

/* estimated ms for n iters at rate iter/s */
static uint32_t est_ms(uint32_t n, uint32_t rate)
{
    if (rate == 0) return 0xFFFFFFFFu;
    return (uint32_t)((uint64_t)n * 1000u / rate);
}

static void print_elapsed(uint32_t ticks)
{
    uint32_t ms = ticks / (COUNT_HZ / 1000u);
    uint32_t m  = ms / 60000u;
    uint32_t s  = (ms % 60000u) / 1000u;
    uint32_t f  = ms % 1000u;
    if (m) printf("%lum%02lu.%03lus", (unsigned long)m, (unsigned long)s, (unsigned long)f);
    else   printf(   "%lu.%03lus",                       (unsigned long)s, (unsigned long)f);
}

static void print_est(uint32_t ms)
{
    if (ms == 0xFFFFFFFFu) { printf("  ???"); return; }
    uint32_t m = ms / 60000u;
    uint32_t s = (ms % 60000u) / 1000u;
    if (m)   printf("%3lum%02lus", (unsigned long)m, (unsigned long)s);
    else     printf("%3lu.%lus",   (unsigned long)s, (unsigned long)((ms % 1000u) / 100u));
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

    uint32_t fcr31 = C1_FCR31();
    C1_WRITE_FCR31(fcr31 &
        ~(C1_ENABLE_OVERFLOW | C1_ENABLE_DIV_BY_0 | C1_ENABLE_INVALID_OP));

    printf("mulmul timing calibration\n\n");
    printf("compute bench: %lu iters...\n", (unsigned long)N_COMPUTE);
    console_render();

    uint32_t ticks_c = bench_compute();
    uint32_t rate_c  = rate_of(N_COMPUTE, ticks_c);

    printf("log bench:     %lu iters...\n", (unsigned long)N_LOG);
    console_render();

    uint32_t ticks_l = bench_log(  );
    uint32_t rate_l  = rate_of(N_LOG, ticks_l);

    /* --- results --- */
    console_clear();
    printf("mulmul timing calibration\n\n");

    printf("compute (no log)  n=%lu\n", (unsigned long)N_COMPUTE);
    printf("  elapsed: ");  print_elapsed(ticks_c);
    printf("  rate: %lu/s\n\n", (unsigned long)rate_c);

    printf("log (all logged)  n=%lu\n", (unsigned long)N_LOG);
    printf("  elapsed: ");  print_elapsed(ticks_l);
    printf("  rate: %lu/s\n\n", (unsigned long)rate_l);

    /* --- estimate table --- */
    static const struct { uint32_t n; const char *label; } rows[] = {
        {      4096, "  4K" },
        {     16384, " 16K" },
        {     40000, " 40K" },
        {    100000, "100K" },
        {    500000, "500K" },
        {   1000000, "  1M" },
        {   8388608, "  8M" },
    };
    const int nrows = (int)(sizeof(rows) / sizeof(rows[0]));

    printf("estimates  (no-log / all-log)\n");
    for (int i = 0; i < nrows; i++) {
        printf("  %s ", rows[i].label);
        print_est(est_ms(rows[i].n, rate_c));
        printf(" / ");
        print_est(est_ms(rows[i].n, rate_l));
        printf("\n");
    }

    console_render();

    debugf("# mulmul_timing results\n");
    debugf("# compute: n=%lu ticks=%lu rate=%lu/s\n",
           (unsigned long)N_COMPUTE, (unsigned long)ticks_c, (unsigned long)rate_c);
    debugf("# log:     n=%lu ticks=%lu rate=%lu/s\n",
           (unsigned long)N_LOG, (unsigned long)ticks_l, (unsigned long)rate_l);

    C1_WRITE_FCR31(fcr31);
    while (1) {}
}