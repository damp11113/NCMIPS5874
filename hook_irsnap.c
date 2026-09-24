/*
 * hook_irsnap: print the IR-related registers in irranges.h (read-only)
 * from inside the stock firmware, right after its input/IR driver is set
 * up. Same line format as irtest.c for diffsnap.py.
 *
 * Patched in with 'go 0x82000000 uio' (hookpatch site after the
 * firmware's "uio init end" message), so the $s2 printf that hook_entry.S
 * passes is not valid there: use the firmware printf that prints
 * "ir_value=0x%x" instead.
 */
#include "irranges.h"

typedef unsigned int u32;
typedef int (*printf_t) (const char *fmt, ...);

#define REG32(addr) (*(volatile u32 *) (addr))
#define FW_PRINTF   ((printf_t) 0x8018c184)

void hook_main (void) {
    printf_t pf = FW_PRINTF;
    u32 r, i;

    pf ("\n=== IR SNAP BEGIN (firmware) ===\n");
    for (r = 0; r < IR_NRANGES; r++) {
        for (i = 0; i < ir_ranges[r].words; i += 4) {
            u32 a = ir_ranges[r].start + i * 4;
            pf ("S %08x: %08x %08x %08x %08x\n", a,
                REG32 (a), REG32 (a + 4), REG32 (a + 8), REG32 (a + 12));
        }
    }
    pf ("=== IR SNAP END ===\n");
}
