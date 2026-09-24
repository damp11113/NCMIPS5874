/*
 * hookpatch: U-Boot program that patches the loaded original firmware so it
 * calls hook_dump.bin (at 0x80004000) once its display is running.
 *
 * Needs, already in RAM: firmware at 0x80008000 (loadimg), hook_dump.bin at
 * 0x80004000 (fatload). Built for 0x82000000 so it does not overlap either.
 * After it returns, 'go 0x80008000' starts the patched firmware.
 *
 *   go 0x82000000        hook at printf ("app_entry_task start"), $s2 = printf
 *   go 0x82000000 uio    hook after the input/IR driver setup, replaces
 *                        printf ("uio init end") at 0x8017e378 (AM_Input_Init)
 *   go 0x82000000 irkey  hook on every remote key press (IR key callback
 *                        0x8017e598), needs a hook built with hook_entry_irkey.S
 */
#include "uboot.h"

#define HOOK_SITE   0x8009a264
#define HOOK_CODE   0x80004000
#define HOOK_SIZE   0x3000
#define HOOK_MAGIC  0x4b4f4f48

#define JAL(target) (0x0c000000u | (((target) >> 2) & 0x03ffffffu))
#define J(target)   (0x08000000u | (((target) >> 2) & 0x03ffffffu))

/* Original firmware instructions at HOOK_SITE: printf ("app_entry_task start") */
static const u32 expect[3] = { 0x3c048004, 0x0240f809, 0x2484ee44 };

/* AM_Input_Init: jalr v1 (printf "uio init end"); addiu a0 in delay slot */
#define UIO_SITE    0x8017e378
static const u32 expect_uio[2] = { 0x0060f809, 0x2484d500 };

/* IR key callback (a0 = user code, a1 = key): addiu sp,-24; li v0,0xfe01 */
#define IRKEY_SITE  0x8017e598
static const u32 expect_irkey[2] = { 0x27bdffe8, 0x3402fe01 };

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

static int hook_present (void) {
    if (REG32 (HOOK_CODE + 12) != HOOK_MAGIC) {
        printf ("hookpatch: hook binary not found at 0x%08x\n", HOOK_CODE);
        return 0;
    }
    return 1;
}

/* Check two firmware words, then replace them (keep = leave word 1 alone) */
static int patch_two (u32 addr, const u32 *want, u32 new0, u32 new1, int keep1, const char *name) {
    volatile u32 *site = (volatile u32 *) addr;
    int i;

    for (i = 0; i < 2; i++) {
        if (site[i] != want[i]) {
            printf ("hookpatch: firmware not found at 0x%08x (word %d = %08x, want %08x)\n",
                    addr + i * 4, i, site[i], want[i]);
            return 1;
        }
    }
    if (!hook_present ()) {
        return 1;
    }

    site[0] = new0;
    if (!keep1) {
        site[1] = new1;
    }

    cache_sync (HOOK_CODE, HOOK_SIZE);
    cache_sync (addr, 8);

    printf ("hookpatch: 0x%08x (%s) now calls 0x%08x. Start firmware with: go 0x80008000\n",
            addr, name, HOOK_CODE);
    return 0;
}

int main (int argc, char *argv[]) {
    volatile u32 *site = (volatile u32 *) HOOK_SITE;
    int i;

    if (argc > 1 && strcmp (argv[1], "uio") == 0) {
        /* delay slot (a0 = string) kept */
        return patch_two (UIO_SITE, expect_uio, JAL (HOOK_CODE), 0, 1, "uio init end");
    }
    if (argc > 1 && strcmp (argv[1], "irkey") == 0) {
        /* hook_entry_irkey.S re-runs the two replaced instructions */
        return patch_two (IRKEY_SITE, expect_irkey, J (HOOK_CODE), 0, 0, "IR key callback");
    }

    for (i = 0; i < 3; i++) {
        if (site[i] != expect[i]) {
            printf ("hookpatch: firmware not found at 0x%08x (word %d = %08x, want %08x)\n",
                    HOOK_SITE + i * 4, i, site[i], expect[i]);
            printf ("  run 'loadimg 1 lzma 0x300000 0x81500000 0x80008000' first\n");
            return 1;
        }
    }
    if (!hook_present ()) {
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
