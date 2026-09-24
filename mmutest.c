/*
 * mmutest: first steps with the 24KEc's MMU (64-entry TLB) and our own
 * exception handling. Everything else in this project runs in the
 * unmapped segments (kseg0 0x80000000 cached, kseg1 0xa0000000 uncached);
 * only addresses below 0x80000000 (kuseg) go through the TLB.
 *
 *   go ${a}              same as "info"
 *   go ${a} info         CP0 state left by U-Boot + valid TLB entries
 *   go ${a} trap         own exception vectors: NULL read, unaligned read,
 *                        break, syscall -> caught and recovered
 *   go ${a} tlb          map a virtual page onto RAM and check both views,
 *                        read-only page (write caught), demand paging
 *   go ${a} user         run a tiny program in user mode in its own pages:
 *                        syscalls for output, killed when it touches kernel
 *                        memory
 *   go ${a} all          trap + tlb + user
 *
 * Each test saves EBase / Status / Wired / EntryHi and puts them back
 * before returning to U-Boot; interrupts stay off while our vectors are
 * installed. The TLB, which U-Boot leaves full of power-on garbage, is
 * flushed first and left clean (see tlb_flush_all). Exception entry:
 * mmu_entry.S.
 * Build: ./buildc.sh "mmutest.c mmu_entry.S" mmutest
 *
 * Physical RAM used for test pages: 0x04100000-0x04140000 (free, below
 * the video write-back buffers at 0x0469dc00).
 */
#include "uboot.h"

#define KSEG0(pa)       (0x80000000u | (pa))

/* TLB entries we use (top of the 64) */
#define IDX_FIRST       56


#define PAGE            0x1000u
#define TEST_PHYS       0x04100000u     /* 64 KB pool of physical pages */
#define DEMAND_VA       0x00500000u     /* demand-paged region */
#define DEMAND_PAGES    16
#define USER_VA         0x00400000u
#define USER_STACK_VA   0x7fffe000u

/* EntryLo: PFN << 6 | C << 3 | D << 2 | V << 1 | G */
#define LO_CACHED       (3u << 3)       /* cacheable, non-coherent, write-back */
#define LO_D            (1u << 2)       /* dirty = writable */
#define LO_V            (1u << 1)
#define LO_G            (1u << 0)       /* global: ignore ASID */
#define ENTRYLO(pa, flags) ((((pa) >> 12) << 6) | (flags))

struct frame {
    u32 r[32];
    u32 hi, lo, epc, status, cause, badvaddr;
};

extern struct frame exc_frame;
extern char exc_base[];
extern char user_prog[], user_prog_end[];
void enter_user (u32 pc, u32 sp);

#define mfc0(reg, sel) ({ u32 __v; \
    __asm__ volatile ("mfc0 %0, $" #reg ", " #sel : "=r" (__v)); __v; })
#define mtc0(reg, sel, v) \
    __asm__ volatile ("mtc0 %0, $" #reg ", " #sel "\n\tehb" : : "r" ((u32) (v)) : "memory")

static const char *exc_names[32] = {
    "Int", "Mod (write to read-only page)", "TLBL (load/fetch miss)", "TLBS (store miss)",
    "AdEL (address error load/fetch)", "AdES (address error store)", "IBE", "DBE",
    "Sys (syscall)", "Bp (break)", "RI (reserved instruction)", "CpU", "Ov", "Tr", "?14",
    "FPE", "?16", "?17", "C2E", "?19", "?20", "?21", "MDMX", "WATCH", "MCheck (machine check)",
    "Thread", "DSPDis", "?27", "?28", "?29", "CacheErr", "?31",
};

/* ---- saved state ---- */

static u32 saved_ebase, saved_status, saved_wired, saved_entryhi, saved_pagemask;

static void tlb_read (int idx, u32 *hi, u32 *lo0, u32 *lo1, u32 *mask) {
    mtc0 (0, 0, idx);
    __asm__ volatile ("tlbr\n\tehb" : : : "memory");
    *hi = mfc0 (10, 0);
    *lo0 = mfc0 (2, 0);
    *lo1 = mfc0 (3, 0);
    *mask = mfc0 (5, 0);
}

static void tlb_write (int idx, u32 hi, u32 lo0, u32 lo1, u32 mask) {
    mtc0 (0, 0, idx);
    mtc0 (10, 0, hi);
    mtc0 (2, 0, lo0);
    mtc0 (3, 0, lo1);
    mtc0 (5, 0, mask);
    __asm__ volatile ("tlbwi\n\tehb" : : : "memory");
}

/* Index of the entry matching va (ASID 0), or -1 */
static int tlb_probe (u32 va) {
    u32 idx;

    mtc0 (10, 0, va & ~0x1fffu);
    __asm__ volatile ("tlbp\n\tehb" : : : "memory");
    idx = mfc0 (0, 0);
    return (idx & 0x80000000u) ? -1 : (int) (idx & 63);
}

/*
 * U-Boot never initialises the TLB: after power-on it holds random
 * entries (random tags, invalid page masks, G set) that can match kuseg
 * addresses, and two matching entries give a machine check. So the whole
 * TLB is rewritten with invalid 4 KB entries whose tags lie in kseg0
 * (never translated) and are chosen so that no other entry, including
 * garbage with huge masks, covers them. U-Boot itself does not use the
 * TLB, so the clean state is left in place afterwards.
 */
static u32 tlb_tag[64], tlb_span[64];   /* current tag and ~(mask | 0x1fff) */

static int tag_conflict (int skip, u32 tag) {
    int j;

    for (j = 0; j < 64; j++) {
        if (j != skip && ((tag ^ tlb_tag[j]) & tlb_span[j]) == 0) {
            return 1;
        }
    }
    return 0;
}

static void tlb_flush_all (void) {
    int i;

    for (i = 0; i < 64; i++) {
        u32 hi, lo0, lo1, mask;

        tlb_read (i, &hi, &lo0, &lo1, &mask);
        tlb_tag[i] = hi & ~0x1fffu;
        tlb_span[i] = ~(mask | 0x1fffu);
    }
    for (i = 0; i < 64; i++) {
        u32 tag = 0x80000000u + (u32) i * 0x2000u;

        while (tag_conflict (i, tag)) {
            tag += 0x80000u;            /* next candidate, still in kseg0 */
        }
        tlb_write (i, tag, 0, 0, 0);
        tlb_tag[i] = tag;
        tlb_span[i] = ~0x1fffu;
    }
    mtc0 (6, 0, 0);                     /* Wired */
}

static void tlb_invalidate (int idx) {
    u32 tag = 0x80000000u + (u32) idx * 0x2000u;

    while (tag_conflict (idx, tag)) {
        tag += 0x80000u;
    }
    tlb_write (idx, tag, 0, 0, 0);
    tlb_tag[idx] = tag;
    tlb_span[idx] = ~0x1fffu;
}

/* Map the even/odd 4 KB page pair containing va (lo = 0: page invalid) */
static void tlb_map_pair (int idx, u32 va, u32 lo0, u32 lo1) {
    int old = tlb_probe (va);

    if (old >= 0 && old != idx) {
        tlb_invalidate (old);           /* avoid a duplicate (machine check) */
    }
    tlb_write (idx, va & ~0x1fffu, lo0, lo1, 0);
    tlb_tag[idx] = va & ~0x1fffu;
    tlb_span[idx] = ~0x1fffu;
}

static void cache_sync (u32 start, u32 len) {
    u32 a;

    for (a = start & ~31u; a < start + len; a += 32) {
        __asm__ volatile (
            "cache 0x15, 0(%0)\n\t"     /* D: hit writeback invalidate */
            "cache 0x10, 0(%0)"         /* I: hit invalidate */
            : : "r" (a) : "memory");
    }
    __asm__ volatile ("sync\n\tehb" : : : "memory");
}

#define ST_IE   (1u << 0)
#define ST_EXL  (1u << 1)
#define ST_ERL  (1u << 2)
#define ST_KSU  (3u << 3)
#define ST_TS   (1u << 21)
#define ST_BEV  (1u << 22)

/* EBase may only change while Status.BEV = 1 (24K manual) */
static void set_ebase (u32 base, u32 status_after) {
    u32 st = mfc0 (12, 0) & ~ST_IE;

    mtc0 (12, 0, st);
    mtc0 (12, 0, st | ST_BEV);
    mtc0 (15, 1, base);
    mtc0 (12, 0, status_after);
}

static void install (void) {
    saved_ebase = mfc0 (15, 1);
    saved_status = mfc0 (12, 0);
    saved_wired = mfc0 (6, 0);
    saved_entryhi = mfc0 (10, 0);
    saved_pagemask = mfc0 (5, 0);

    printf ("  install: interrupts off, EBase 0x%08x -> 0x%08x\n", saved_ebase, (u32) exc_base);
    set_ebase ((u32) exc_base, saved_status & ~(ST_IE | ST_EXL | ST_ERL | ST_KSU | ST_BEV));
    printf ("  install: vectors live, flushing TLB\n");
    tlb_flush_all ();
    printf ("  install: TLB clean\n");
}

static void uninstall (void) {
    mtc0 (5, 0, saved_pagemask);
    mtc0 (10, 0, saved_entryhi & 0xff);
    set_ebase (saved_ebase, saved_status);
}

/* ---- exception handling ---- */

enum { MODE_RECOVER, MODE_DEMAND, MODE_USER };

/* Shared with the exception handler: volatile, or the compiler may keep
 * stale copies in registers across the (invisible) exceptions */
static volatile int mode;
static volatile u32 last_code, last_epc, last_badvaddr, exc_count;
static void *recover_jb[5], *user_jb[5];
static volatile int recover_armed;             /* recover_jb holds a valid context */
static volatile int user_exit_code;

/* Demand paging state */
static volatile u32 demand_faults, demand_victim;

static void recover (void) {
    __builtin_longjmp (recover_jb, 1);
}

static void user_return (void) {
    __builtin_longjmp (user_jb, 1);
}

/* Leave the exception into kernel code at fn () (EXL cleared by eret) */
static void resume_at (struct frame *f, void (*fn) (void)) {
    f->epc = (u32) fn;
    f->status &= ~0x18u;                /* KSU = kernel */
}

static void demand_fault (struct frame *f) {
    u32 va = f->badvaddr & ~0x1fffu;    /* even/odd pair */
    u32 pair = (va - DEMAND_VA) / (2 * PAGE);
    u32 pa = TEST_PHYS + 0x10000u + pair * 2 * PAGE;
    int idx = tlb_probe (va);

    if (idx < 0) {                      /* refill: take the next of 4 slots */
        idx = IDX_FIRST + 4 + (demand_victim++ & 3);
    }
    tlb_map_pair (idx, va, ENTRYLO (pa, LO_CACHED | LO_D | LO_V | LO_G),
                  ENTRYLO (pa + PAGE, LO_CACHED | LO_D | LO_V | LO_G));
    demand_faults++;
    /* EPC unchanged: the load / store runs again and now hits */
}

void exc_handler (struct frame *f) {
    u32 code = (f->cause >> 2) & 31;
    int from_user = (f->status & 0x18u) == 0x10u;

    exc_count++;
    if (code == 24) {
        f->status &= ~ST_TS;            /* machine check: TLB shutdown flag */
    }
    last_code = code;
    last_epc = f->epc;
    last_badvaddr = f->badvaddr;

    if (mode == MODE_DEMAND && (code == 2 || code == 3) &&
        f->badvaddr >= DEMAND_VA && f->badvaddr < DEMAND_VA + DEMAND_PAGES * PAGE) {
        demand_fault (f);
        return;
    }

    if (mode == MODE_USER && from_user) {
        if (code == 8) {                /* syscall */
            u32 num = f->r[2], arg = f->r[4];

            f->epc += 4;
            if (num == 1) {
                puts ((const char *) arg);          /* user va: mapped, readable here */
            } else if (num == 2) {
                printf ("  [user] count %d\n", arg);
            } else if (num == 3) {
                user_exit_code = arg;
                resume_at (f, user_return);
            } else {
                printf ("  [kernel] unknown syscall %d\n", num);
            }
            return;
        }
        printf ("  [kernel] user program killed: %s at pc 0x%08x, address 0x%08x\n",
                exc_names[code], f->epc, f->badvaddr);
        user_exit_code = -1;
        resume_at (f, user_return);
        return;
    }

    if ((mode == MODE_RECOVER || mode == MODE_DEMAND) && recover_armed) {
        recover_armed = 0;
        resume_at (f, recover);
        return;
    }

    /* No recovery point: say what happened, then stop (power cycle) */
    printf ("  [kernel] unexpected %s at pc 0x%08x (addr 0x%08x, status 0x%08x), halted\n",
            exc_names[code], f->epc, f->badvaddr, f->status);
    for (;;) {
    }
}

/* ---- tests ---- */

static void __attribute__ ((noinline)) do_null_read (void) {
    volatile u32 *p = (volatile u32 *) 0;

    (void) *p;
}

/* Plain lw in asm: from C, GCC sees the odd constant address and uses the
 * lwl/lwr unaligned pair, which never faults */
static void __attribute__ ((noinline)) do_unaligned_read (void) {
    u32 v;

    __asm__ volatile ("lw %0, 0(%1)\n\tnop" : "=r" (v) : "r" (0x80100001u) : "memory");
    (void) v;
}

static void __attribute__ ((noinline)) do_break (void) {
    __asm__ volatile ("break 7\n\tnop");
}

static void __attribute__ ((noinline)) do_kernel_syscall (void) {
    __asm__ volatile ("syscall\n\tnop");
}

static int expect (const char *what, void (*fn) (void), u32 want_code) {
    int ok;

    exc_count = 0;
    mode = MODE_RECOVER;
    if (__builtin_setjmp (recover_jb) == 0) {
        recover_armed = 1;
        fn ();
        printf ("  %-26s no exception (FAIL)\n", what);
        return 0;
    }
    ok = last_code == want_code;
    printf ("  %-26s %s, pc 0x%08x, addr 0x%08x -> recovered %s\n", what,
            exc_names[last_code], last_epc, last_badvaddr, ok ? "OK" : "(unexpected code)");
    return ok;
}

static void test_trap (void) {
    int ok = 0;

    printf ("trap: own vectors at EBase 0x%08x\n", (u32) exc_base);
    install ();
    ok += expect ("NULL read", do_null_read, 2);
    ok += expect ("unaligned read", do_unaligned_read, 4);
    ok += expect ("break", do_break, 9);
    ok += expect ("syscall (kernel mode)", do_kernel_syscall, 8);
    uninstall ();
    printf ("trap: %d / 4 OK, U-Boot vectors restored\n", ok);
}

static void test_tlb (void) {
    volatile u32 *virt = (volatile u32 *) USER_VA;
    volatile u32 *phys = (volatile u32 *) KSEG0 (TEST_PHYS);
    u32 i, ok = 1, sum = 0;

    printf ("tlb: map virtual 0x%08x -> physical 0x%08x (TLB entry %d)\n",
            USER_VA, TEST_PHYS, IDX_FIRST);
    install ();

    tlb_map_pair (IDX_FIRST, USER_VA, ENTRYLO (TEST_PHYS, LO_CACHED | LO_D | LO_V | LO_G),
                  ENTRYLO (TEST_PHYS + PAGE, LO_CACHED | LO_D | LO_V | LO_G));
    for (i = 0; i < 2048; i++) {                    /* both pages */
        virt[i] = 0xa5000000u + i;
    }
    for (i = 0; i < 2048; i++) {
        if (phys[i] != 0xa5000000u + i) {
            ok = 0;
        }
    }
    phys[5] = 0x12345678u;
    printf ("  write via virtual, read via physical: %s; write physical 0x12345678, "
            "virtual reads 0x%08x %s\n", ok ? "OK" : "FAIL", virt[5],
            virt[5] == 0x12345678u ? "OK" : "FAIL");

    /* Read-only: same pages, D = 0 */
    tlb_map_pair (IDX_FIRST, USER_VA, ENTRYLO (TEST_PHYS, LO_CACHED | LO_V | LO_G),
                  ENTRYLO (TEST_PHYS + PAGE, LO_CACHED | LO_V | LO_G));
    mode = MODE_RECOVER;
    if (__builtin_setjmp (recover_jb) == 0) {
        recover_armed = 1;
        u32 v = virt[7];

        printf ("  read-only page: read 0x%08x OK, now writing...\n", v);
        virt[7] = 1;
        printf ("  read-only page: write went through (FAIL)\n");
    } else {
        printf ("  read-only page: write caught: %s at 0x%08x -> %s\n", exc_names[last_code],
                last_badvaddr, last_code == 1 ? "OK" : "unexpected");
    }

    /* Demand paging: 16 pages behind 4 TLB entries (8 pages) */
    mode = MODE_DEMAND;
    demand_faults = 0;
    demand_victim = 0;
    if (__builtin_setjmp (recover_jb) == 0) {
        recover_armed = 1;
        volatile u32 *d = (volatile u32 *) DEMAND_VA;

        for (i = 0; i < DEMAND_PAGES; i++) {
            d[i * PAGE / 4] = 0xd0000000u + i;      /* first touch: fault + map */
        }
        printf ("  demand paging: wrote %d pages, %d faults\n", DEMAND_PAGES, demand_faults);
        demand_faults = 0;
        for (i = 0; i < DEMAND_PAGES; i++) {
            u32 v = d[i * PAGE / 4];

            sum += v == 0xd0000000u + i;
        }
        printf ("  demand paging: read back %d / %d correct, %d faults (evicted pages "
                "mapped again, data kept) %s\n", sum, DEMAND_PAGES, demand_faults,
                sum == DEMAND_PAGES ? "OK" : "FAIL");
    } else {
        printf ("  demand paging: unexpected %s at 0x%08x (addr 0x%08x)\n",
                exc_names[last_code], last_epc, last_badvaddr);
    }

    uninstall ();
    printf ("tlb: done, U-Boot vectors restored, TLB left clean\n");
}

static void test_user (void) {
    u32 code_pa = TEST_PHYS, stack_pa = TEST_PHYS + 2 * PAGE;
    u32 len = user_prog_end - user_prog, i;
    unsigned char *dst = (unsigned char *) KSEG0 (code_pa);

    printf ("user: %d-byte program -> virtual 0x%08x, stack at 0x%08x\n", len, USER_VA,
            USER_STACK_VA);
    for (i = 0; i < len; i++) {
        dst[i] = user_prog[i];
    }
    cache_sync (KSEG0 (code_pa), len);

    install ();
    /* Code page read-only (D = 0), stack page writable; both user-visible
     * because they are in kuseg. The kernel's own memory (kseg0/1) is
     * off limits to user mode by the CPU itself. */
    tlb_map_pair (IDX_FIRST, USER_VA, ENTRYLO (code_pa, LO_CACHED | LO_V | LO_G), 0);
    tlb_map_pair (IDX_FIRST + 1, USER_STACK_VA, ENTRYLO (stack_pa, LO_CACHED | LO_D | LO_V | LO_G),
                  ENTRYLO (stack_pa + PAGE, LO_CACHED | LO_D | LO_V | LO_G));

    mode = MODE_USER;
    exc_count = 0;
    user_exit_code = 0;
    if (__builtin_setjmp (user_jb) == 0) {
        enter_user (USER_VA, USER_STACK_VA + 2 * PAGE - 16);
    }
    mode = MODE_RECOVER;
    uninstall ();
    printf ("user: back in kernel mode, exit code %d, %d exceptions handled %s\n",
            user_exit_code, exc_count, user_exit_code == -1 ? "(killed as expected: OK)" : "");
}

static void info (void) {
    u32 st = mfc0 (12, 0), c1 = mfc0 (16, 1), cfg = mfc0 (16, 0);
    u32 entries = ((c1 >> 25) & 63) + 1, hi_save = mfc0 (10, 0), mask_save = mfc0 (5, 0);
    u32 i, valid = 0;

    printf ("CPU PRId 0x%08x, Config 0x%08x (MMU type %d: %s), %d TLB entries\n",
            mfc0 (15, 0), cfg, (cfg >> 7) & 7, ((cfg >> 7) & 7) == 1 ? "TLB" : "?", entries);
    printf ("Status 0x%08x: BEV %d, ERL %d, EXL %d, KSU %d, IE %d, IM 0x%02x\n", st,
            (st >> 22) & 1, (st >> 2) & 1, (st >> 1) & 1, (st >> 3) & 3, st & 1, (st >> 8) & 0xff);
    printf ("EBase 0x%08x, Wired %d, EntryHi 0x%08x (ASID %d), PageMask 0x%08x, "
            "Context 0x%08x\n", mfc0 (15, 1), mfc0 (6, 0), hi_save, hi_save & 0xff,
            mask_save, mfc0 (4, 0));
    printf ("Cause 0x%08x, EPC 0x%08x, ErrorEPC 0x%08x, sp 0x%08x\n", mfc0 (13, 0),
            mfc0 (14, 0), mfc0 (30, 0), ({ u32 s; __asm__ ("move %0, $sp" : "=r" (s)); s; }));

    for (i = 0; i < entries; i++) {
        u32 hi, lo0, lo1, mask;

        tlb_read (i, &hi, &lo0, &lo1, &mask);
        if ((lo0 | lo1) & LO_V) {
            printf ("  TLB %2d: EntryHi 0x%08x Lo0 0x%08x Lo1 0x%08x Mask 0x%08x\n",
                    i, hi, lo0, lo1, mask);
            valid++;
        }
    }
    mtc0 (10, 0, hi_save);
    mtc0 (5, 0, mask_save);
    printf ("%d valid TLB entries (U-Boot and this program run unmapped in kseg0/kseg1)\n",
            valid);
    printf ("Our vectors would go at 0x%08x\n", (u32) exc_base);
}

int main (int argc, char *argv[]) {
    const char *cmd = argc > 1 ? argv[1] : "info";

    if (!strcmp (cmd, "info")) {
        info ();
    } else if (!strcmp (cmd, "trap")) {
        test_trap ();
    } else if (!strcmp (cmd, "tlb")) {
        test_tlb ();
    } else if (!strcmp (cmd, "user")) {
        test_user ();
    } else if (!strcmp (cmd, "all")) {
        test_trap ();
        test_tlb ();
        test_user ();
    } else {
        printf ("usage: go ${a} [info|trap|tlb|user|all]\n");
        return 1;
    }
    return 0;
}
