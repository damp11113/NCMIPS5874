/*
 * ehci.h: own minimal EHCI host driver for one high-speed device on a
 * root port, polled (no interrupts). Made for the IPTV box's second USB
 * controller (0xbf070000, root port 1 = internal RTL8188FTV WiFi module),
 * so the WiFi works while U-Boot keeps the stick on its own controller
 * (0xbf060000, port 0), and so a bulk IN can wait for data without
 * blocking (U-Boot's EHCI waits ~5 s on an empty bulk IN).
 *
 * Controller (both ports, same layout, ChipIdea-style "TDI" EHCI):
 *   base + 0x20  capability registers (CAPLENGTH 0x20, HCSPARAMS 0x10011)
 *   base + 0x40  operational registers: USBCMD +0x00, USBSTS +0x04,
 *                USBINTR +0x08, ASYNCLISTADDR +0x18, CONFIGFLAG +0x40,
 *                PORTSC +0x44, USBMODE +0x68 (3 = host, cleared by reset)
 * U-Boot's ehci_hcd_init writes PORTSC = 0xc0001000 (port power + the top
 * bits) before its reset; we do the same.
 *
 *   ehci_open(h, base)              reset, port reset, enumerate (address 1,
 *                                   configuration 1); 0 / -1
 *   ehci_control(h, req, type, value, index, data, len, ms)
 *                                   bytes moved / -1
 *   ehci_bulk(h, ep, data, len, ms) synchronous bulk, bytes moved / -1
 *   ehci_rx_start(h, ep, n, size)   keep n bulk IN buffers queued on ep
 *   ehci_rx_get(h, &len)            next filled buffer (or 0), never waits
 *   ehci_rx_put(h)                  hand that buffer back to the controller
 *
 * Every endpoint has its own queue head (all on one async ring) and one
 * owner: control, each bulk OUT and the RX ring may be used from
 * different tasks, but one endpoint never from two at once.
 *
 * Descriptors and buffers are in normal (cached) RAM: descriptors are
 * accessed through their uncached kseg1 alias, bulk data is written back /
 * invalidated around each transfer (32-byte cache lines).
 */
#ifndef EHCI_H
#define EHCI_H

#include "uboot.h"

void *memset(void *dst, int c, unsigned int n);

#define EHCI_WIFI_BASE      0xbf070000u     /* IPTV box: root port 1, WiFi */

#define EHCI_CMD            0x40
#define EHCI_STS            0x44
#define EHCI_INTR           0x48
#define EHCI_ASYNC          0x58
#define EHCI_CONFIGFLAG     0x80
#define EHCI_PORTSC         0x84
#define EHCI_USBMODE        0xa8

#define CMD_RUN             (1u << 0)
#define CMD_RESET           (1u << 1)
#define CMD_ASE             (1u << 5)
#define STS_HALTED          (1u << 12)
#define STS_ASS             (1u << 15)
#define PS_CCS              (1u << 0)
#define PS_CSC              (1u << 1)
#define PS_PE               (1u << 2)
#define PS_PEC              (1u << 3)
#define PS_OCC              (1u << 5)
#define PS_PR               (1u << 8)
#define PS_PP               (1u << 12)
#define PS_RWC              (PS_CSC | PS_PEC | PS_OCC)

/* qTD token */
#define TD_ACTIVE           (1u << 7)
#define TD_HALTED           (1u << 6)
#define TD_ERRMASK          0x7cu           /* halted, buffer, babble, xact, missed */
#define TD_PID_OUT          (0u << 8)
#define TD_PID_IN           (1u << 8)
#define TD_PID_SETUP        (2u << 8)
#define TD_CERR3            (3u << 10)
#define TD_TOGGLE           (1u << 31)
#define TD_BYTES(t)         (((t) >> 16) & 0x7fff)
#define TD_T                1u              /* terminate bit of a link */

#define EHCI_NQH            6               /* head, ep0, up to 4 bulk */
#define EHCI_NTD            8               /* qTDs for control / bulk OUT */
#define EHCI_RXMAX          16              /* RX ring qTDs */

/* 32 bytes; no alignment attribute here: it is also the QH overlay at
 * offset 0x10 (the arrays are 32-byte aligned instead) */
struct ehci_td {
    u32 next, alt, token, buf[5];
};

struct ehci_qh {
    u32 link, info1, info2, cur;
    struct ehci_td ov;                      /* overlay (8 words) */
    u32 pad[4];
} __attribute__((aligned(32)));

typedef char ehci_qh_size_check[sizeof(struct ehci_qh) == 64 ? 1 : -1];

struct ehci_ep {
    struct ehci_qh *qh;                     /* uncached pointer */
    u32 ep;                                 /* address incl. 0x80 = IN */
    u32 mps;
};

struct ehci {
    u32 base, addr;
    int nep;
    struct ehci_ep ep0, bulk[4];
    /* RX ring */
    struct ehci_ep *rx_ep;
    int rx_n, rx_next;
    u32 rx_size;
    unsigned char *rx_buf;                  /* cached, rx_n * rx_size */
    u32 rx_errors;
};

/* All controller memory (descriptors 4 KB-aligned pool) */
static struct ehci_qh ehci_qhs[EHCI_NQH] __attribute__((aligned(4096)));
static struct ehci_td ehci_tds[EHCI_NTD + 2 * 4] __attribute__((aligned(32)));
static struct ehci_td ehci_rxtds[EHCI_RXMAX] __attribute__((aligned(32)));
static unsigned char ehci_setup_buf[32] __attribute__((aligned(32)));
static unsigned char ehci_ctl_buf[512] __attribute__((aligned(32)));

#define EHCI_UC(p)          ((void *) (((u32) (p) & 0x1fffffffu) | 0xa0000000u))
#define EHCI_PHYS(p)        (((u32) (p) & 0x1fffffffu) | 0x80000000u)
#define EHCI_R(h, r)        REG32((h)->base + (r))
#define EHCI_W(h, r, v)     (REG32((h)->base + (r)) = (v))

static inline void ehci_sync(void) {
    __asm__ volatile("sync" : : : "memory");
}

/* Write back + invalidate the data cache lines of [p, p + n) */
static inline void ehci_cache_wbinv(const void *p, u32 n) {
    u32 a = (u32) p & ~31u, e = (u32) p + n;

    if ((u32) p >= 0xa0000000u) {
        return;                                 /* already uncached */
    }
    for (; a < e; a += 32) {
        __asm__ volatile("cache 0x15, 0(%0)" : : "r" (a) : "memory");
    }
    ehci_sync();
}

static inline u32 ehci_now_ms(void) {
    return get_timer(0);
}

/* Fill a qTD (uncached pointer td) for len bytes at data (cached or not) */
static inline void ehci_td_fill(struct ehci_td *td, u32 next, u32 token, const void *data, u32 len) {
    u32 p = EHCI_PHYS(data), i;

    td->token = 0;                              /* inactive while it is filled */
    ehci_sync();
    td->next = next;
    td->alt = TD_T;
    td->buf[0] = p;
    p &= ~0xfffu;
    for (i = 1; i < 5; i++) {
        p += 0x1000;
        td->buf[i] = p;
    }
    ehci_sync();
    td->token = token | (len << 16) | TD_CERR3;
}

/* Queue head for endpoint ep (0x80 = IN) with max packet size mps.
 * Control: data toggle from the qTD. Bulk: toggle kept in the overlay. */
static inline void ehci_qh_init(struct ehci *h, struct ehci_ep *e, int idx, u32 ep, u32 mps) {
    struct ehci_qh *q = EHCI_UC(&ehci_qhs[idx]);

    e->qh = q;
    e->ep = ep;
    e->mps = mps;
    q->info1 = h->addr | ((ep & 0x0f) << 8) | (2u << 12) | (mps << 16) | (3u << 28) |
               ((ep & 0x0f) == 0 ? (1u << 14) : 0);
    q->info2 = 1u << 30;                        /* mult 1 */
    q->cur = 0;
    q->ov.next = TD_T;
    q->ov.alt = TD_T;
    q->ov.token = 0;
}

/* Start the queue at first (uncached qTD): only when the QH is idle.
 * Keeps the overlay's data toggle (bulk), clears a halt. */
static inline void ehci_qh_start(struct ehci_ep *e, struct ehci_td *first) {
    struct ehci_qh *q = e->qh;

    q->ov.next = EHCI_PHYS(first);
    q->ov.alt = TD_T;
    ehci_sync();
    q->ov.token &= TD_TOGGLE;
    ehci_sync();
}

/* Wait until td is done. 0 ok, -1 error / timeout (queue stopped) */
static inline int ehci_td_wait(struct ehci_ep *e, struct ehci_td *td, u32 ms) {
    u32 t0 = ehci_now_ms(), tok;

    for (;;) {
        tok = td->token;
        if (!(tok & TD_ACTIVE)) {
            return (tok & TD_ERRMASK) ? -1 : 0;
        }
        if (ehci_now_ms() - t0 > ms) {
            break;
        }
    }
    /* timeout: take the qTD away from the controller. The QH is
     * re-pointed on its next use; mark the qTD inactive so a late
     * completion does not matter. */
    e->qh->ov.next = TD_T;
    td->token &= ~TD_ACTIVE;
    e->qh->ov.token &= ~TD_ACTIVE;
    ehci_sync();
    udelay(200);                                /* one more pass of the async ring */
    return -1;
}

/* Control transfer on endpoint 0: setup + optional data + status */
static inline int ehci_control(struct ehci *h, u32 req, u32 type, u32 value, u32 index,
                               void *data, u32 len, u32 ms) {
    struct ehci_td *s = EHCI_UC(&ehci_tds[0]), *d = EHCI_UC(&ehci_tds[1]),
                   *st = EHCI_UC(&ehci_tds[2]);
    unsigned char *sp = EHCI_UC(ehci_setup_buf), *buf = EHCI_UC(ehci_ctl_buf);
    int in = (type & 0x80) != 0;
    u32 i, got;

    if (len > sizeof(ehci_ctl_buf)) {
        return -1;
    }
    sp[0] = type;
    sp[1] = req;
    sp[2] = value;
    sp[3] = value >> 8;
    sp[4] = index;
    sp[5] = index >> 8;
    sp[6] = len;
    sp[7] = len >> 8;
    if (!in) {
        for (i = 0; i < len; i++) {
            buf[i] = ((unsigned char *) data)[i];
        }
    }
    /* status stage: opposite direction (IN if no data), toggle 1 */
    ehci_td_fill(st, TD_T, TD_ACTIVE | ((in && len) ? TD_PID_OUT : TD_PID_IN) | TD_TOGGLE, 0, 0);
    if (len) {
        ehci_td_fill(d, EHCI_PHYS(st), TD_ACTIVE | (in ? TD_PID_IN : TD_PID_OUT) | TD_TOGGLE,
                     buf, len);
        if (in) {
            d->alt = EHCI_PHYS(st);             /* short read: go on to the status */
        }
    }
    ehci_td_fill(s, EHCI_PHYS(len ? d : st), TD_ACTIVE | TD_PID_SETUP, sp, 8);
    ehci_sync();
    ehci_qh_start(&h->ep0, s);
    if (ehci_td_wait(&h->ep0, st, ms) < 0) {
        return -1;
    }
    if (!len) {
        return 0;
    }
    got = len - TD_BYTES(d->token);
    if (in) {
        for (i = 0; i < got; i++) {
            ((unsigned char *) data)[i] = buf[i];
        }
    }
    return got;
}

static inline struct ehci_ep *ehci_find_ep(struct ehci *h, u32 ep) {
    int i;

    for (i = 0; i < h->nep; i++) {
        if (h->bulk[i].ep == ep) {
            return &h->bulk[i];
        }
    }
    return 0;
}

/* Synchronous bulk transfer (OUT, or IN with a timeout), one qTD:
 * len <= 16 KB from any address (20 KB if 4 KB aligned). Each OUT
 * endpoint gets its own qTD, so two tasks may send on different ones. */
static inline int ehci_bulk(struct ehci *h, u32 ep, void *data, u32 len, u32 ms) {
    struct ehci_ep *e = ehci_find_ep(h, ep);
    struct ehci_td *td;
    int in = (ep & 0x80) != 0;

    if (!e || len > 16384) {
        return -1;
    }
    td = EHCI_UC(&ehci_tds[EHCI_NTD + 2 * (e - h->bulk)]);
    ehci_cache_wbinv(data, len);
    ehci_td_fill(td, TD_T, TD_ACTIVE | (in ? TD_PID_IN : TD_PID_OUT), data, len);
    ehci_sync();
    ehci_qh_start(e, td);
    if (ehci_td_wait(e, td, ms) < 0) {
        return -1;
    }
    if (in) {
        ehci_cache_wbinv(data, len);
    }
    return len - TD_BYTES(td->token);
}

/* ---- RX ring: n qTDs in a circle, each with its own buffer ---- */

static inline void ehci_rx_arm(struct ehci *h, int i) {
    struct ehci_td *td = EHCI_UC(&ehci_rxtds[i]);
    unsigned char *b = h->rx_buf + i * h->rx_size;

    ehci_cache_wbinv(b, h->rx_size);            /* no dirty line may land on new data */
    ehci_td_fill(td, EHCI_PHYS(&ehci_rxtds[(i + 1) % h->rx_n]), TD_ACTIVE | TD_PID_IN, b,
                 h->rx_size);
}

/* buf: n * size bytes, 4 KB aligned, size a multiple of 4 KB, <= 16 KB */
static inline int ehci_rx_start(struct ehci *h, u32 ep, int n, u32 size, unsigned char *buf) {
    struct ehci_ep *e = ehci_find_ep(h, ep);
    int i;

    if (!e || n > EHCI_RXMAX || n < 2) {
        return -1;
    }
    h->rx_ep = e;
    h->rx_n = n;
    h->rx_size = size;
    h->rx_buf = buf;
    h->rx_next = 0;
    for (i = 0; i < n; i++) {
        ehci_rx_arm(h, i);
    }
    ehci_sync();
    ehci_qh_start(e, EHCI_UC(&ehci_rxtds[0]));
    return 0;
}

/* Next completed RX buffer (cached address) and its length, or 0 */
static inline unsigned char *ehci_rx_get(struct ehci *h, u32 *len) {
    struct ehci_td *td = EHCI_UC(&ehci_rxtds[h->rx_next]);
    u32 tok = td->token;
    unsigned char *b = h->rx_buf + h->rx_next * h->rx_size;

    if (tok & TD_ACTIVE) {
        return 0;
    }
    if (tok & TD_ERRMASK) {
        /* the queue halted on this qTD: re-arm it and restart there */
        h->rx_errors++;
        ehci_rx_arm(h, h->rx_next);
        ehci_sync();
        ehci_qh_start(h->rx_ep, td);
        return 0;
    }
    *len = h->rx_size - TD_BYTES(tok);
    ehci_cache_wbinv(b, *len);
    return b;
}

/* Done with the buffer ehci_rx_get returned: give it back */
static inline void ehci_rx_put(struct ehci *h) {
    struct ehci_qh *q = h->rx_ep->qh;
    int i = h->rx_next;

    ehci_rx_arm(h, i);
    ehci_sync();
    h->rx_next = (i + 1) % h->rx_n;
    /* the controller stops at an inactive qTD; if it stopped on this one
     * (ring was full) its overlay still points here and it picks it up
     * on its next pass. If the overlay shows a stop with "next =
     * terminate" (should not happen in a ring), restart. */
    if ((q->ov.next & TD_T) && !(q->ov.token & TD_ACTIVE)) {
        ehci_qh_start(h->rx_ep, EHCI_UC(&ehci_rxtds[i]));
    }
}

/* ---- controller + device bring-up ---- */

static inline int ehci_wait_reg(struct ehci *h, u32 reg, u32 mask, u32 val, u32 ms) {
    u32 t0 = ehci_now_ms();

    while ((EHCI_R(h, reg) & mask) != val) {
        if (ehci_now_ms() - t0 > ms) {
            return -1;
        }
    }
    return 0;
}

/* Reset the controller, power + reset the port, set up the async ring */
static inline int ehci_hc_init(struct ehci *h) {
    struct ehci_qh *head = EHCI_UC(&ehci_qhs[0]);
    u32 v;
    int i;

    EHCI_W(h, EHCI_PORTSC, 0xc0001000u);            /* as U-Boot's ehci_hcd_init */
    EHCI_W(h, EHCI_CMD, (EHCI_R(h, EHCI_CMD) & ~(CMD_RUN | CMD_ASE)));
    ehci_wait_reg(h, EHCI_STS, STS_HALTED, STS_HALTED, 20);
    EHCI_W(h, EHCI_CMD, CMD_RESET);
    if (ehci_wait_reg(h, EHCI_CMD, CMD_RESET, 0, 250) < 0) {
        printf("ehci: controller reset timeout\n");
        return -1;
    }
    EHCI_W(h, EHCI_USBMODE, EHCI_R(h, EHCI_USBMODE) | 3);   /* host mode */
    EHCI_W(h, EHCI_INTR, 0);

    /* the pools are only used through kseg1 from now on: no dirty cache
     * line (e.g. from zeroing .bss) may be written back over them later */
    ehci_cache_wbinv(ehci_qhs, sizeof(ehci_qhs));
    ehci_cache_wbinv(ehci_tds, sizeof(ehci_tds));
    ehci_cache_wbinv(ehci_rxtds, sizeof(ehci_rxtds));
    ehci_cache_wbinv(ehci_setup_buf, sizeof(ehci_setup_buf));
    ehci_cache_wbinv(ehci_ctl_buf, sizeof(ehci_ctl_buf));

    for (i = 0; i < EHCI_NQH; i++) {
        struct ehci_qh *q = EHCI_UC(&ehci_qhs[i]);

        q->link = TD_T;
        q->info1 = q->info2 = q->cur = 0;
        q->ov.next = q->ov.alt = TD_T;
        q->ov.token = 0;
    }
    /* head of reclamation list, halted, ring = head only for now */
    head->link = EHCI_PHYS(&ehci_qhs[0]) | 2;
    head->info1 = (1u << 15) | (2u << 12);
    head->ov.token = TD_HALTED;
    ehci_sync();
    EHCI_W(h, EHCI_ASYNC, EHCI_PHYS(&ehci_qhs[0]));
    v = EHCI_R(h, EHCI_CMD) & ~(CMD_RESET | CMD_ASE);
    EHCI_W(h, EHCI_CMD, v | CMD_RUN);
    EHCI_W(h, EHCI_CONFIGFLAG, 1);
    (void) EHCI_R(h, EHCI_CMD);
    udelay(5000);

    /* port power, reset (50 ms) */
    v = EHCI_R(h, EHCI_PORTSC) & ~PS_RWC;
    EHCI_W(h, EHCI_PORTSC, v | PS_PP);
    /* after the controller reset the connect shows up only after ~100 ms */
    if (ehci_wait_reg(h, EHCI_PORTSC, PS_CCS, PS_CCS, 1500) == 0) {
        udelay(100000);                             /* USB connect debounce */
    }
    if (!(EHCI_R(h, EHCI_PORTSC) & PS_CCS)) {
        printf("ehci: nothing connected (PORTSC %08x)\n", EHCI_R(h, EHCI_PORTSC));
        return -1;
    }
    v = EHCI_R(h, EHCI_PORTSC) & ~(PS_RWC | PS_PE);
    EHCI_W(h, EHCI_PORTSC, v | PS_PR);
    udelay(50000);
    EHCI_W(h, EHCI_PORTSC, EHCI_R(h, EHCI_PORTSC) & ~(PS_RWC | PS_PR));
    if (ehci_wait_reg(h, EHCI_PORTSC, PS_PR, 0, 20) < 0) {
        printf("ehci: port reset does not end\n");
        return -1;
    }
    udelay(10000);
    v = EHCI_R(h, EHCI_PORTSC);
    if (!(v & PS_PE) || ((v >> 26) & 3) != 2) {
        printf("ehci: port not enabled at high speed (PORTSC %08x)\n", v);
        return -1;
    }
    return 0;
}

/* Link the endpoint QHs into the async ring and start it */
static inline void ehci_ring_start(struct ehci *h) {
    int i, n = 2 + h->nep;

    for (i = 0; i < n; i++) {
        struct ehci_qh *q = EHCI_UC(&ehci_qhs[i]);

        q->link = EHCI_PHYS(&ehci_qhs[(i + 1) % n]) | 2;
    }
    ehci_sync();
    EHCI_W(h, EHCI_CMD, EHCI_R(h, EHCI_CMD) | CMD_ASE);
    ehci_wait_reg(h, EHCI_STS, STS_ASS, STS_ASS, 20);
}

/*
 * Bring up the controller at base and the device on its port: address 1,
 * configuration 1, a queue head for every bulk endpoint of interface 0.
 * vid / pid (0 = any) are checked. Returns 0 / -1 (message printed).
 */
static inline int ehci_open(struct ehci *h, u32 base, u32 vid, u32 pid) {
    static unsigned char cfg[512];
    unsigned char d[18];
    int n, i;
    u32 mps0, total;

    memset(h, 0, sizeof(*h));
    h->base = base;
    if (ehci_hc_init(h) < 0) {
        return -1;
    }
    /* address 0, max packet 64 for the first descriptor read */
    h->addr = 0;
    ehci_qh_init(h, &h->ep0, 1, 0, 64);
    h->nep = 0;
    ehci_ring_start(h);

    n = ehci_control(h, 6, 0x80, 0x0100, 0, d, 18, 500);       /* GET_DESCRIPTOR device */
    if (n < 8) {
        struct ehci_td *t = EHCI_UC(ehci_tds);

        printf("ehci: device descriptor read failed (%d): qTD tokens %08x %08x %08x, "
               "QH overlay %08x next %08x, USBCMD %08x USBSTS %08x PORTSC %08x\n", n,
               t[0].token, t[1].token, t[2].token, h->ep0.qh->ov.token, h->ep0.qh->ov.next,
               EHCI_R(h, EHCI_CMD), EHCI_R(h, EHCI_STS), EHCI_R(h, EHCI_PORTSC));
        return -1;
    }
    mps0 = d[7];
    if (ehci_control(h, 5, 0x00, 1, 0, 0, 0, 500) < 0) {          /* SET_ADDRESS 1 */
        printf("ehci: SET_ADDRESS failed\n");
        return -1;
    }
    udelay(10000);
    h->addr = 1;
    h->ep0.qh->info1 = (h->ep0.qh->info1 & ~(0x7fu | (0x7ffu << 16))) | 1 | (mps0 << 16);
    n = ehci_control(h, 6, 0x80, 0x0100, 0, d, 18, 500);
    if (n < 18) {
        printf("ehci: descriptor read at address 1 failed (%d)\n", n);
        return -1;
    }
    if ((vid && (d[8] | d[9] << 8) != vid) || (pid && (d[10] | d[11] << 8) != pid)) {
        printf("ehci: device %04x:%04x, not the one expected\n", d[8] | d[9] << 8,
               d[10] | d[11] << 8);
        return -1;
    }
    printf("ehci: device %04x:%04x at %08x, ep0 max packet %d\n", d[8] | d[9] << 8,
           d[10] | d[11] << 8, base, mps0);

    /* configuration descriptor: 9 bytes for the total, then all of it */
    if (ehci_control(h, 6, 0x80, 0x0200, 0, d, 9, 500) < 9) {
        return -1;
    }
    total = d[2] | d[3] << 8;
    if (total > sizeof(cfg)) {
        total = sizeof(cfg);
    }
    n = ehci_control(h, 6, 0x80, 0x0200, 0, cfg, total, 500);
    if (n < 9) {
        return -1;
    }
    for (i = 0; i + 2 <= n && cfg[i] >= 2; i += cfg[i]) {
        if (cfg[i + 1] == 4 && cfg[i + 2] != 0) {
            break;                                  /* only interface 0 */
        }
        if (cfg[i + 1] == 5 && (cfg[i + 3] & 3) == 2 && h->nep < 4) {
            u32 ep = cfg[i + 2], mps = cfg[i + 4] | (cfg[i + 5] & 7) << 8;

            ehci_qh_init(h, &h->bulk[h->nep], 2 + h->nep, ep, mps);
            h->nep++;
        }
    }
    if (ehci_control(h, 9, 0x00, cfg[5], 0, 0, 0, 500) < 0) {  /* SET_CONFIGURATION */
        printf("ehci: SET_CONFIGURATION failed\n");
        return -1;
    }
    /* add the bulk QHs to the ring (async off while relinking) */
    EHCI_W(h, EHCI_CMD, EHCI_R(h, EHCI_CMD) & ~CMD_ASE);
    ehci_wait_reg(h, EHCI_STS, STS_ASS, 0, 20);
    ehci_ring_start(h);
    for (i = 0; i < h->nep; i++) {
        printf("ehci: bulk ep %02x max packet %d\n", h->bulk[i].ep, h->bulk[i].mps);
    }
    return 0;
}

/* Stop the controller (device stays powered) */
static inline void ehci_close(struct ehci *h) {
    if (!h->base) {
        return;
    }
    EHCI_W(h, EHCI_CMD, EHCI_R(h, EHCI_CMD) & ~(CMD_ASE | CMD_RUN));
    ehci_wait_reg(h, EHCI_STS, STS_HALTED, STS_HALTED, 20);
}

#endif
