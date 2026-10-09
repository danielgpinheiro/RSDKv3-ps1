/*
 * PC sampler (`make SAMPLE=1`, docs/28 speed pass 2; after RSDKv5-ps1's, docs/32 3.3e), on the stock 2 MB console.
 * Root counter 0 counts the system clock and raises its IRQ every PS1_SAMPLE_PERIOD cycles (psx-spx "Timers": mode
 * bit 3 reset at target, bit 4 IRQ at target, bit 6 repeat; source 0 = system clock). The first handler of the BIOS
 * IRQ chain acknowledges it and, while g_ps1SampleOn is set, appends one sample to a 4 KB ring: the interrupted
 * instruction (EPC, COP0 r14: nothing raises another exception inside the handler) and the script VM's instruction
 * (g_ps1ScriptPC: the scriptCode index ProcessScript runs, 0 outside it; RSDKv3/Script.cpp). Mania's per-instruction
 * histogram needs 8 MB, which the user ruled out (stock RAM / VRAM only): here each half of the ring, once full, is
 * handed to PS1SampleDrain, where GDB stops and appends it to a file (tools/play/scene_run.sh); the emulated time does
 * not move while GDB reads. tools/play/sample_report.py maps the samples to functions and to scripts.
 */
#include <stdint.h>

#if PS1_SAMPLE
#include "common/psxlibc/handlers.h"
#include "common/syscalls/syscalls.h"

#define PS1_SAMPLE_PERIOD 9973 // cycles between samples (prime: no lock-step with a loop or the hblank)
#define PS1_SAMPLE_HALF   256  // samples in half the ring

extern "C" {
volatile uint32_t g_ps1SampleOn     = 0; // GDB: 1 = take samples
volatile uint32_t g_ps1SampleCount  = 0; // samples taken (total)
volatile uint32_t g_ps1SamplePeriod = PS1_SAMPLE_PERIOD;
volatile uint32_t g_ps1SampleRing[2 * PS1_SAMPLE_HALF * 2]; // (EPC, script PC) pairs
volatile uint32_t g_ps1SamplePos   = 0; // next sample in the ring
volatile uint32_t g_ps1SampleHalf  = 0; // the half PS1SampleDrain hands over (0 / 1)
volatile uint32_t g_ps1SampleDrains = 0;
}
extern volatile int32_t g_ps1ScriptPC;

// GDB breaks here: g_ps1SampleRing[g_ps1SampleHalf * 512 ...] holds 256 complete samples.
extern "C" __attribute__((noinline)) void PS1SampleDrain() { g_ps1SampleDrains = g_ps1SampleDrains + 1; }

static int pcSample() {
    volatile uint32_t *istat = (volatile uint32_t *)0xBF801070, *imask = (volatile uint32_t *)0xBF801074;
    if (!(*istat & *imask & 0x10))
        return 0;
    *istat = ~0x10u; // 0 acknowledges, 1 leaves the other bits alone
    if (g_ps1SampleOn) {
        uint32_t epc, pos = g_ps1SamplePos;
        __asm__ volatile("mfc0 %0, $14" : "=r"(epc));
        g_ps1SampleRing[pos * 2]     = epc;
        g_ps1SampleRing[pos * 2 + 1] = (uint32_t)g_ps1ScriptPC;
        g_ps1SampleCount             = g_ps1SampleCount + 1;
        if (++pos == 2 * PS1_SAMPLE_HALF)
            pos = 0;
        g_ps1SamplePos = pos;
        if (pos == 0 || pos == PS1_SAMPLE_HALF) {
            g_ps1SampleHalf = pos ? 0 : 1;
            PS1SampleDrain();
        }
    }
    return 0;
}
static HandlerInfo s_sampler = {nullptr, nullptr, pcSample, 0};

void PS1SamplerInstall() {
    syscall_sysEnqIntRP(0, &s_sampler);
    volatile uint16_t *t0 = (volatile uint16_t *)0xBF801100;
    t0[4] = PS1_SAMPLE_PERIOD; // target
    t0[2] = 0x0058;            // mode: system clock, reset at target, IRQ at target, repeat, pulse
    *(volatile uint32_t *)0xBF801074 |= 0x10;
}
#else
void PS1SamplerInstall() {}
#endif
