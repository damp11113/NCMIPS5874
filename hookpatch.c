/*
 * hookpatch: U-Boot program that patches the loaded original firmware so it
 * calls hook_dump.bin (at 0x80004000) once its display is running.
 *
 * Needs, already in RAM: firmware at 0x80008000 (loadimg), hook_dump.bin at
 * 0x80004000 (fatload). Built for 0x82000000 so it does not overlap either.
 * After it returns, 'go 0x80008000' starts the patched firmware.
 */
#include "uboot.h"

#define HOOK_SITE   0x8009a264
#define HOOK_CODE   0x80004000
#define HOOK_SIZE   0x3000
#define HOOK_MAGIC  0x4b4f4f48

#define JAL(target) (0x0c000000u | (((target) >> 2) & 0x03ffffffu))

/* Original firmware instructions at HOOK_SITE: printf ("app_entry_task start") */
static const u32 expect[3] = { 0x3c048004, 0x0240f809, 0x2484ee44 };

static void cache_sync (u32 start, u32 len) {
    u32 a;

    for (a = start & ~31u; a < start + len; a += 32) {
        __asm__ volatile (
            "cache 0x15, 0(%0)\n\t"     /* D: hit writeback invalidate */
            "cache 0x10, 0(%0)"         /* I: hit invalidate */
            : : "r" (a) : "memory");
    }
    __asm__ volatile ("sync" : : : "memory");
}

int main (int argc, char *argv[]) {
    volatile u32 *site = (volatile u32 *) HOOK_SITE;
    int i;

    for (i = 0; i < 3; i++) {
        if (site[i] != expect[i]) {
            printf ("hookpatch: firmware not found at 0x%08x (word %d = %08x, want %08x)\n",
                    HOOK_SITE + i * 4, i, site[i], expect[i]);
            printf ("  run 'loadimg 1 lzma 0x300000 0x81500000 0x80008000' first\n");
            return 1;
        }
    }
    if (REG32 (HOOK_CODE + 12) != HOOK_MAGIC) {
        printf ("hookpatch: hook_dump.bin not found at 0x%08x\n", HOOK_CODE);
        return 1;
    }

    site[0] = JAL (HOOK_CODE);
    site[1] = 0;    /* nop (delay slot) */
    site[2] = 0;    /* nop */

    cache_sync (HOOK_CODE, HOOK_SIZE);
    cache_sync (HOOK_SITE, 12);

    printf ("hookpatch: 0x%08x now calls 0x%08x. Start firmware with: go 0x80008000\n",
            HOOK_SITE, HOOK_CODE);
    return 0;
}
