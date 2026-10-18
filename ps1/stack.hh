#pragma once
#include <stdint.h>

// The main stack, moved out of the heap (CD-092, ../docs/44). nugget's allocator sizes the heap up to __stack_start, the
// RAM top the BIOS stack grows down from: malloc never failed while the heap's last free block still overlapped the
// live stack, and the R51A time-warp load's 32 KB stream windows were handed out on top of it (the stack then wrote
// over the free list). main() runs the app on this .bss array instead: the heap owns everything up to __stack_start,
// malloc returns NULL at the true end (the windows fall back to 1-sector reads, as designed), and psyqo_heap_end() /
// g_ps1HeapHeadroom count bytes that are really free. psyqo's exception handler has its own stack (vector.s, 0x1000).
#define PS1_STACK_BYTES (16 * 1024) // deepest stack measured 4.3 KB (R11A / SS1 / R51A, ../tests cd-probe-stack-*)

extern uint32_t g_ps1Stack[PS1_STACK_BYTES / 4];
extern volatile uint32_t g_ps1StackOverflow; // frames whose check found the stack's bottom guard words overwritten

int PS1RunOnStack(int (*fn)(), void *top); // calls fn with $sp = top (8-byte aligned), returns its result
void PS1StackPaint();                        // fill the array with the paint word (call before PS1RunOnStack)
bool PS1StackGuardBroken();                  // the bottom guard words are no longer paint
uint32_t PS1StackLowWater();                 // lowest address the stack has written (scan; for checkpoints / GDB)
