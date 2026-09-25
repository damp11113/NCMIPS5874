// SPDX-License-Identifier: GPL-2.0-only
/*
 * Derived from Linux drivers/net/wireless/realtek/rtl8xxxu (core.c, 8188f.c,
 * 8723b.c): Copyright (c) 2014 - 2017 Jes Sorensen, (c) 2022 Bitterblue
 * Smith, portions (c) Realtek Corporation.
 *
 * RTL8188FTV (USB WiFi, 0bda:f179, internal module on root port 1) driver
 * pieces, ported onto U-Boot's USB stack (ubusb.h). Run 'usb port 1;
 * usb reset' first.
 *
 * Register access: vendor control request 0x05, type 0xc0 read / 0x40
 * write, value = register address, index 0.
 *
 * VERIFIED 2026-09-24: register reads, eFuse (MAC b4:6d:c2:24:84:20),
 * power on + firmware (rtlfw), full init + channel scan (rtlscan),
 * management frame TX (rtlprobe: probe responses received).
 */
#ifndef RTL8188_H
#define RTL8188_H

#include "uboot.h"
#include "ubusb.h"

#define RTL_VID                 0x0bda

#define REG_SYS_ISO_CTRL        0x0000
#define REG_SYS_FUNC            0x0002
#define  SYS_FUNC_CPU_ENABLE    (1u << 10)
#define REG_APS_FSMCO           0x0004
#define  APS_FSMCO_MAC_ENABLE   (1u << 8)
#define  APS_FSMCO_SW_LPS       (1u << 10)
#define  APS_FSMCO_HW_SUSPEND   (1u << 11)
#define  APS_FSMCO_PCIE         (1u << 12)
#define  APS_FSMCO_HW_POWERDOWN (1u << 15)
#define REG_SYS_CLKR            0x0008
#define REG_RSV_CTRL            0x001c
#define REG_EFUSE_CTRL          0x0030
#define REG_MCU_FW_DL           0x0080
#define  MCU_FW_DL_ENABLE       (1u << 0)
#define  MCU_FW_DL_READY        (1u << 1)
#define  MCU_FW_DL_CSUM_REPORT  (1u << 2)
#define  MCU_WINT_INIT_READY    (1u << 6)
#define  MCU_FW_RAM_SEL         (1u << 7)
#define REG_SYS_CFG             0x00f0
#define REG_CR                  0x0100
#define REG_HMTFR               0x01cc
#define REG_FW_START_ADDRESS    0x1000

#define RTL_FW_PAGE_SIZE        4096
#define RTL_FW_HEADER_SIZE      32
#define RTL_WRITEN_BLOCK        128         /* 8188F writeN_block_size */
#define RTL_MAX_REG_POLL        500
#define RTL_FW_POLL_MAX         1000
#define RTL_EFUSE_LEN           512

static void *rtl;
static unsigned char rtl_io[RTL_WRITEN_BLOCK] __attribute__ ((aligned (32)));
static unsigned char rtl_efuse[RTL_EFUSE_LEN];

/* ---- register access -------------------------------------------------- */

static inline int rtl_read (u32 reg, int len, u32 *val) {
    int i;

    if (ub_control (rtl, 0x05, 0xc0, reg, 0, rtl_io, len, 500) < 0) {
        return -1;
    }
    *val = 0;
    for (i = len - 1; i >= 0; i--) {
        *val = (*val << 8) | rtl_io[i];
    }
    return 0;
}

static inline int rtl_write (u32 reg, int len, u32 val) {
    int i;

    for (i = 0; i < len; i++) {
        rtl_io[i] = val >> (8 * i);
    }
    return ub_control (rtl, 0x05, 0x40, reg, 0, rtl_io, len, 500) < 0 ? -1 : 0;
}

static inline u32 rd8 (u32 reg) {
    u32 v = 0xea;

    rtl_read (reg, 1, &v);
    return v;
}

static inline u32 rd16 (u32 reg) {
    u32 v = 0xeaea;

    rtl_read (reg, 2, &v);
    return v;
}

static inline u32 rd32 (u32 reg) {
    u32 v = 0xeaeaeaea;

    rtl_read (reg, 4, &v);
    return v;
}

#define wr8(r, v)   rtl_write ((r), 1, (v))
#define wr16(r, v)  rtl_write ((r), 2, (v))
#define wr32(r, v)  rtl_write ((r), 4, (v))

/* Block write in 128-byte control transfers (rtl8xxxu_writeN) */
static inline int rtl_writeN (u32 addr, const unsigned char *buf, u32 len) {
    while (len) {
        u32 k = len > RTL_WRITEN_BLOCK ? RTL_WRITEN_BLOCK : len, i;

        for (i = 0; i < k; i++) {
            rtl_io[i] = buf[i];
        }
        if (ub_control (rtl, 0x05, 0x40, addr, 0, rtl_io, k, 500) != (int) k) {
            return -1;
        }
        addr += k;
        buf += k;
        len -= k;
    }
    return 0;
}

/* Find the chip; returns 0 or -1 */
static inline int rtl_open (void) {
    int i;

    for (i = 0; i < UB_USB_MAX_DEVICE; i++) {
        void *dev = ub_usb_dev (i);

        if (dev && UB_DEV_VID (dev) == RTL_VID) {
            rtl = dev;
            return 0;
        }
    }
    return -1;
}

/* ---- eFuse (rtl8xxxu_read_efuse / read_efuse8) ------------------------ */

static inline int efuse_read8 (u32 addr) {
    u32 v = rd32 (REG_EFUSE_CTRL);
    int i;

    v &= ~(0xffu | (0x3ffu << 8) | 0x80000000u);
    v |= (addr & 0x3ff) << 8;
    wr32 (REG_EFUSE_CTRL, v);
    for (i = 0; i < 1000; i++) {
        v = rd32 (REG_EFUSE_CTRL);
        if (v & 0x80000000u) {
            return v & 0xff;
        }
    }
    return -1;
}

/* Fills rtl_efuse (8188F layout: VID/PID @0xd0, MAC @0xd7). Returns the
 * number of raw bytes used, or -1. */
static inline int rtl_read_efuse (void) {
    u32 v, addr = 0, map, i;
    int header, h2, offset;

    v = rd16 (REG_SYS_ISO_CTRL);
    if (!(v & 0x8000)) {
        wr16 (REG_SYS_ISO_CTRL, v | 0x8000);
    }
    v = rd16 (REG_SYS_FUNC);
    if (!(v & 0x1000)) {
        wr16 (REG_SYS_FUNC, v | 0x1000);
    }
    v = rd16 (REG_SYS_CLKR);
    if (!(v & 0x0020) || !(v & 0x0002)) {
        wr16 (REG_SYS_CLKR, v | 0x0022);
    }

    for (i = 0; i < RTL_EFUSE_LEN; i++) {
        rtl_efuse[i] = 0xff;
    }
    while (addr < RTL_EFUSE_LEN) {
        header = efuse_read8 (addr++);
        if (header < 0) {
            return -1;
        }
        if (header == 0xff) {
            break;
        }
        if ((header & 0x1f) == 0x0f) {
            offset = (header & 0xe0) >> 5;
            h2 = efuse_read8 (addr++);
            if ((h2 & 0x0f) == 0x0f) {
                continue;
            }
            offset |= (h2 & 0xf0) >> 1;
            header = h2;
        } else {
            offset = header >> 4;
        }
        map = offset * 8;
        for (i = 0; i < 4; i++) {
            if (!(header & (1 << i))) {
                if (map + 1 < RTL_EFUSE_LEN) {
                    rtl_efuse[map] = efuse_read8 (addr++);
                    rtl_efuse[map + 1] = efuse_read8 (addr++);
                } else {
                    addr += 2;
                }
            }
            map += 2;
        }
    }
    return addr;
}

/* ---- power on (rtl8188fu_power_on) ------------------------------------ */

static inline int rtl_power_on (void) {
    u32 v;
    int n;

    /* disabled_to_emu: enable WL suspend bits off, USB APHY LDO on */
    v = rd8 (REG_APS_FSMCO + 1);
    v &= ~((APS_FSMCO_PCIE | APS_FSMCO_HW_SUSPEND) >> 8);
    wr8 (REG_APS_FSMCO + 1, v);
    wr8 (0xc4, rd8 (0xc4) & ~0x10u);

    /* emu_to_active */
    wr8 (REG_APS_FSMCO + 1, rd8 (REG_APS_FSMCO + 1) & ~(APS_FSMCO_SW_LPS >> 8));
    for (n = RTL_MAX_REG_POLL; n; n--) {            /* power ready */
        if (rd32 (REG_APS_FSMCO) & (1u << 17)) {
            break;
        }
        udelay (10);
    }
    if (!n) {
        return -1;
    }
    wr8 (REG_APS_FSMCO + 1, rd8 (REG_APS_FSMCO + 1) & ~(APS_FSMCO_HW_POWERDOWN >> 8));
    wr8 (REG_APS_FSMCO + 1, rd8 (REG_APS_FSMCO + 1) & ~(APS_FSMCO_HW_SUSPEND >> 8));
    wr8 (REG_APS_FSMCO + 1, rd8 (REG_APS_FSMCO + 1) | (APS_FSMCO_MAC_ENABLE >> 8));
    for (n = RTL_MAX_REG_POLL; n; n--) {            /* MAC enable self-clears */
        if (!(rd32 (REG_APS_FSMCO) & APS_FSMCO_MAC_ENABLE)) {
            break;
        }
        udelay (10);
    }
    if (!n) {
        return -2;
    }
    wr8 (0x27, 0x35);                               /* reduce RF noise */

    /* CR: HCI TX/RX DMA, TX/RX DMA, protocol, scheduler, security, cal timer */
    wr8 (REG_CR, 0);
    wr16 (REG_CR, rd16 (REG_CR) | 0x063f);
    return 0;
}

/* ---- firmware (rtl8xxxu_download_firmware / start_firmware) ----------- */

static inline void rtl_reset_8051 (void) {
    u32 sys_func;

    wr8 (REG_RSV_CTRL + 1, rd8 (REG_RSV_CTRL + 1) & ~1u);
    sys_func = rd16 (REG_SYS_FUNC) & ~SYS_FUNC_CPU_ENABLE;
    wr16 (REG_SYS_FUNC, sys_func);
    wr8 (REG_RSV_CTRL + 1, rd8 (REG_RSV_CTRL + 1) | 1u);
    wr16 (REG_SYS_FUNC, sys_func | SYS_FUNC_CPU_ENABLE);
}

/* fw = whole rtl8188fufw.bin (with 32-byte header), size = file size */
static inline int rtl_download_firmware (const unsigned char *fw, u32 size) {
    const unsigned char *p = fw + RTL_FW_HEADER_SIZE;
    u32 len = size - RTL_FW_HEADER_SIZE, page = 0, ret = 0;

    wr8 (REG_SYS_FUNC + 1, rd8 (REG_SYS_FUNC + 1) | 4);
    wr16 (REG_SYS_FUNC, rd16 (REG_SYS_FUNC) | SYS_FUNC_CPU_ENABLE);

    if (rd8 (REG_MCU_FW_DL) & MCU_FW_RAM_SEL) {     /* already running */
        wr8 (REG_MCU_FW_DL, 0);
        rtl_reset_8051 ();
    }
    wr8 (REG_MCU_FW_DL, rd8 (REG_MCU_FW_DL) | MCU_FW_DL_ENABLE);
    wr32 (REG_MCU_FW_DL, rd32 (REG_MCU_FW_DL) & ~(1u << 19));      /* 8051 reset */
    wr8 (REG_MCU_FW_DL, rd8 (REG_MCU_FW_DL) | MCU_FW_DL_CSUM_REPORT);

    while (len) {
        u32 k = len > RTL_FW_PAGE_SIZE ? RTL_FW_PAGE_SIZE : len;

        wr8 (REG_MCU_FW_DL + 2, (rd8 (REG_MCU_FW_DL + 2) & 0xf8) | page);
        if (rtl_writeN (REG_FW_START_ADDRESS, p, k) < 0) {
            ret = -1;
            break;
        }
        p += k;
        len -= k;
        page++;
    }

    wr16 (REG_MCU_FW_DL, rd16 (REG_MCU_FW_DL) & ~MCU_FW_DL_ENABLE);
    return ret;
}

/* Returns 0 when the firmware reports ready, -1 checksum, -2 no start */
static inline int rtl_start_firmware (void) {
    u32 v;
    int i;

    for (i = 0; i < RTL_FW_POLL_MAX; i++) {
        if (rd32 (REG_MCU_FW_DL) & MCU_FW_DL_CSUM_REPORT) {
            break;
        }
    }
    if (i == RTL_FW_POLL_MAX) {
        return -1;
    }

    v = rd32 (REG_MCU_FW_DL);
    v |= MCU_FW_DL_READY;
    v &= ~MCU_WINT_INIT_READY;
    wr32 (REG_MCU_FW_DL, v);
    rtl_reset_8051 ();

    for (i = 0; i < RTL_FW_POLL_MAX; i++) {
        if (rd32 (REG_MCU_FW_DL) & MCU_WINT_INIT_READY) {
            break;
        }
        udelay (100);
    }
    if (i == RTL_FW_POLL_MAX) {
        return -2;
    }
    wr8 (REG_HMTFR, 0x0f);                          /* init H2C command */
    return 0;
}

/* ---- RF registers (rtl8xxxu_read_rfreg / write_rfreg, path A only) ---- */

#include "rtl8188_tables.h"

#define REG_FPGA0_XA_HSSI_PARM1     0x0820
#define REG_FPGA0_XA_HSSI_PARM2     0x0824
#define REG_FPGA0_XA_LSSI_PARM      0x0840
#define REG_FPGA0_XA_LSSI_READBACK  0x08a0
#define REG_HSPI_XA_READBACK        0x08b8

static inline u32 rf_read (u32 reg) {
    u32 hssia = rd32 (REG_FPGA0_XA_HSSI_PARM2), v = hssia;

    v &= ~0x7f800000u;
    v |= reg << 23;
    v |= 0x80000000u;                               /* EDGE_READ */
    hssia &= ~0x80000000u;
    wr32 (REG_FPGA0_XA_HSSI_PARM2, hssia);
    udelay (10);
    wr32 (REG_FPGA0_XA_HSSI_PARM2, v);
    udelay (100);
    hssia |= 0x80000000u;
    wr32 (REG_FPGA0_XA_HSSI_PARM2, hssia);
    udelay (10);
    if (rd32 (REG_FPGA0_XA_HSSI_PARM1) & (1u << 8)) {    /* PI mode */
        return rd32 (REG_HSPI_XA_READBACK) & 0xfffff;
    }
    return rd32 (REG_FPGA0_XA_LSSI_READBACK) & 0xfffff;
}

static inline void rf_write (u32 reg, u32 data) {
    wr32 (REG_FPGA0_XA_LSSI_PARM, (reg << 20) | (data & 0xfffff));
    udelay (1);
}

/* ---- init tables ------------------------------------------------------- */

static inline void rtl_write_mac_table (const struct rtl_reg8val *t) {
    for (; !(t->reg == 0xffff && t->val == 0xff); t++) {
        wr8 (t->reg, t->val);
    }
}

static inline void rtl_write_phy_table (const struct rtl_reg32val *t) {
    for (; !(t->reg == 0xffff && t->val == 0xffffffff); t++) {
        wr32 (t->reg, t->val);
        udelay (1);
    }
}

static inline void rtl_write_rf_table (const struct rtl_rfregval *t) {
    for (; !(t->reg == 0xff && t->val == 0xffffffff); t++) {
        switch (t->reg) {
        case 0xfe: udelay (50000); continue;
        case 0xfd: udelay (5000); continue;
        case 0xfc: udelay (1000); continue;
        case 0xfb: udelay (50); continue;
        case 0xfa: udelay (5); continue;
        case 0xf9: udelay (1); continue;
        }
        rf_write (t->reg, t->val);
        udelay (1);
    }
}

/* ---- full device init (rtl8xxxu_init_device for the 8188F) ------------ */

#define TX_TOTAL_PAGE_NUM_8188F     0xf7
#define TX_PAGE_NUM_HI_PQ_8188F     0x0c
#define TX_PAGE_NUM_NORM_PQ_8188F   0x02

/* Antenna selection: rtl8723bu_phy_init_antenna_selection (8723b.c),
 * which Linux also uses for the 8188F */
static inline void rtl_antenna_selection (void) {
    wr32 (0x0064, rd32 (0x0064) & ~((1u << 20) | (1u << 24)));  /* PAD_CTRL1 */
    wr32 (0x0040, rd32 (0x0040) & ~(1u << 4));                  /* GPIO_MUXCFG */
    wr32 (0x0040, rd32 (0x0040) | (1u << 3));
    wr32 (0x004c, rd32 (0x004c) | (1u << 24));                  /* LEDCFG0 */
    wr32 (0x004c, rd32 (0x004c) & ~(1u << 23));
    wr32 (0x0944, rd32 (0x0944) | 0x3);                         /* RFE_BUFFER */
    wr32 (0x0930, (rd32 (0x0930) & 0xffffff00u) | 0x77);        /* RFE_CTRL_ANTA_SRC */
    wr32 (0x0038, rd32 (0x0038) | (1u << 11));                  /* PWR_DATA: RFE ctrl */
}

/* Steps as in rtl8xxxu_init_device + rtl8188fu_fops; the TX-only parts
 * (EDCA, retry, CAM, TX report) and IQ calibration are left out.
 * Returns 0 or a negative step number. */
/*
 * Power off as Linux rtl8188fu_power_off (active_to_lps, active_to_emu,
 * emu_to_disabled). The WiFi module keeps its power across a box reset, so
 * after an earlier run the chip is still on with its firmware running, and
 * power-on failed ("power ready" / MAC enable never settle).
 */
static inline void rtl_power_off_to (int disable) {
    int n;

    wr16 (0x0040, rd16 (0x0040) & ~(1u << 12));     /* GPIO_MUXCFG */
    wr32 (0x00b4, 0xffffffffu);                     /* HISR0 */
    wr32 (0x00bc, 0xffffffffu);                     /* HISR1 */
    wr8 (0x04ec, rd8 (0x04ec) & ~(1u << 1));        /* TX report timer off */
    wr8 (0x001f, 0);                                /* RF_CTRL: RF off */
    if (rd8 (REG_MCU_FW_DL) & MCU_FW_RAM_SEL) {     /* firmware self reset */
        wr8 (REG_HMTFR + 3, 0x20);
        for (n = 100; n && (rd16 (REG_SYS_FUNC) & SYS_FUNC_CPU_ENABLE); n--) {
            udelay (50);
        }
        if (!n) {
            wr16 (REG_SYS_FUNC, rd16 (REG_SYS_FUNC) & ~SYS_FUNC_CPU_ENABLE);
        }
    }
    /* active_to_lps */
    wr8 (0x0138 + 1, rd8 (0x0138 + 1) | 1);         /* FTIMR: CPWM */
    wr8 (0x0522, 0xff);                             /* TXPAUSE */
    for (n = 100; n && rd32 (0x05f8); n--) {        /* SCH_TX_CMD: TX idle */
    }
    wr8 (REG_SYS_FUNC, rd8 (REG_SYS_FUNC) & ~1u);   /* BBRSTB */
    udelay (2);
    wr8 (REG_SYS_FUNC, rd8 (REG_SYS_FUNC) & ~2u);   /* BB_GLB_RSTN */
    wr16 (REG_CR, (rd16 (REG_CR) | 0x3f) & ~((1u << 6) | (1u << 7) | (1u << 9)));
    wr8 (0x0553, rd8 (0x0553) | (1u << 5));         /* DUAL_TSF_RST: TX OK */
    /* reset MCU, MCU ready status */
    wr16 (REG_SYS_FUNC, rd16 (REG_SYS_FUNC) & ~SYS_FUNC_CPU_ENABLE);
    wr8 (REG_MCU_FW_DL, 0);
    /* active_to_emu */
    wr8 (0x001f, 0);
    wr8 (0x4e, rd8 (0x4e) & ~0x80u);
    wr8 (0x27, 0x34);
    wr8 (REG_APS_FSMCO + 1, rd8 (REG_APS_FSMCO + 1) | ((1u << 9) >> 8));   /* MAC off */
    for (n = RTL_MAX_REG_POLL; n && (rd32 (REG_APS_FSMCO) & (1u << 9)); n--) {
        udelay (10);
    }
    if (!disable) {
        return;                                     /* keep USB alive (retry) */
    }
    /* emu_to_disabled: WL suspend, USB APHY LDO off - the chip leaves USB */
    wr8 (REG_APS_FSMCO + 1, (rd8 (REG_APS_FSMCO + 1) &
                             ~((APS_FSMCO_PCIE | APS_FSMCO_HW_SUSPEND) >> 8)) |
                            (APS_FSMCO_HW_SUSPEND >> 8));
    wr8 (0xc4, rd8 (0xc4) | 0x10);
}

/* Full Linux power off (the chip then drops off USB until a bus reset) */
__attribute__ ((unused))
static inline void rtl_power_off (void) {
    rtl_power_off_to (1);
}

static inline int rtl_init_device (const unsigned char *fw, u32 fw_size) {
    u32 v, pubq;
    int n;

    if (rd8 (REG_MCU_FW_DL) & MCU_FW_RAM_SEL) {
        printf ("rtl: chip still running from an earlier start, MAC off first\n");
        rtl_power_off_to (0);
    }
    n = rtl_power_on ();
    if (n < 0) {
        /* left in some other state by an earlier start: MAC off (the full
         * power off makes the chip leave USB), then on again */
        printf ("rtl: power on failed (%s), MAC off and retrying\n",
                n == -1 ? "power ready" : "MAC enable");
        rtl_power_off_to (0);
        udelay (10000);
        n = rtl_power_on ();
    }
    if (n < 0) {
        printf ("rtl: power on failed (%s)\n", n == -1 ? "power ready" : "MAC enable");
        return -1;
    }

    /* init_queue_reserved_page: 2 OUT endpoints = high + normal queue */
    wr32 (0x0214, TX_PAGE_NUM_NORM_PQ_8188F);               /* RQPN_NPQ */
    pubq = TX_TOTAL_PAGE_NUM_8188F - TX_PAGE_NUM_HI_PQ_8188F - TX_PAGE_NUM_NORM_PQ_8188F - 1;
    wr32 (0x0200, 0x80000000u | TX_PAGE_NUM_HI_PQ_8188F | (pubq << 16));    /* RQPN */

    /* init_queue_priority (2 endpoints: hi = HIGH 3, lo = NORMAL 2) */
    v = rd16 (0x010c) & 0x7;
    v |= (3 << 4) | (3 << 6) | (2 << 8) | (2 << 10) | (3 << 12) | (3 << 14);
    wr16 (0x010c, v);

    wr16 (0x0114 + 2, 0x3f7f);                              /* RX page boundary */

    if (rtl_download_firmware (fw, fw_size) < 0) {
        return -2;
    }
    if (rtl_start_firmware () < 0) {
        return -3;
    }

    rtl_antenna_selection ();
    rtl_write_mac_table (rtl8188f_mac_init_table);

    /* init_phy_bb (rtl8188fu_init_phy_bb) */
    wr16 (REG_SYS_FUNC, rd16 (REG_SYS_FUNC) | 0x0003 | 0x2000);    /* BB_GLB_RSTN, BBRSTB, DIO_RF */
    wr8 (0x001f, 0x07);                                     /* RF_CTRL: enable, rstb, sdmrstb */
    udelay (20);
    rf_write (0x01, 0x780);                                 /* IQADJ_G1 */
    wr8 (REG_SYS_FUNC, 0x03 | 0x04 | 0x10);                 /* BB rst, USBA, USBD */
    rtl_write_phy_table (rtl8188fu_phy_init_table);
    rtl_write_phy_table (rtl8188f_agc_table);

    /* crystal cap from eFuse xtal_k (rtl8188f_set_crystal_cap) */
    v = rd32 (0x0024) & ~((0x3fu << 17) | (0x3fu << 11));
    v |= ((rtl_efuse[0xb9] & 0x3fu) << 17) | ((rtl_efuse[0xb9] & 0x3fu) << 11);
    wr32 (0x0024, v);

    /* init_phy_rf (rtl8xxxu_init_phy_rf, path A) */
    {
        u32 rfenv = rd16 (0x0870) & 0x10;

        wr32 (0x0860, rd32 (0x0860) | (1u << 20));
        udelay (1);
        wr32 (0x0860, rd32 (0x0860) | (1u << 4));
        udelay (1);
        wr32 (0x0824, rd32 (0x0824) & ~0x400u);             /* 3-wire addr len */
        udelay (1);
        wr32 (0x0824, rd32 (0x0824) & ~0x800u);             /* 3-wire data len */
        udelay (1);
        if (((rd32 (REG_SYS_CFG) >> 12) & 0xf) == 1) {
            rtl_write_rf_table (rtl8188fu_cut_b_radioa_init_table);
        } else {
            rtl_write_rf_table (rtl8188fu_radioa_init_table);
        }
        wr16 (0x0870, (rd16 (0x0870) & ~0x10u) | rfenv);
    }

    wr32 (0x0804, 0x00000003);                              /* FPGA0_TX_INFO */
    /* XAB_RF_SW_CTRL: TRSW, TRSWB, ANTSW, ANTSWB (+ board ctrl), PAPE */
    v = (1u << 5) | (1u << 6) | (1u << 8) | (1u << 9) | (((1u << 8) | (1u << 9)) << 16) |
        (1u << 10) | ((1u << 10) << 16);
    wr32 (0x0870, v);
    wr32 (0x0860, 0x66f60210);                              /* XA_RF_INT_OE */

    /* TX buffer boundary */
    v = TX_TOTAL_PAGE_NUM_8188F + 1;
    wr8 (0x0424, v);
    wr8 (0x0425, v);
    wr8 (0x045d, v);
    wr8 (0x0114, v);
    wr8 (0x0208 + 1, v);
    wr8 (0x0104, 0x22);                                     /* PBP: 256 B pages */

    /* auto LLT */
    wr32 (0x0224, rd32 (0x0224) | (1u << 16));
    for (n = 500; n; n--) {
        if (!(rd32 (0x0224) & (1u << 16))) {
            break;
        }
        udelay (3);
    }
    if (!n) {
        return -4;
    }

    /* usb_quirks: MAC TX/RX enable, drop bad TX data */
    wr16 (REG_CR, rd16 (REG_CR) | (1u << 6) | (1u << 7));
    wr32 (0x020c, rd32 (0x020c) | (1u << 9));

    wr8 (0x060f, 4);                                        /* RX_DRVINFO_SZ: PHY status */
    wr32 (0x00b4, 0xffffffff);                              /* HISR0 */
    wr32 (0x00bc, 0xffffffff);                              /* HISR1 */

    /* RCR as Linux + accept all unicast (monitor) */
    wr32 (0x0608, (1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 13) |
          (1u << 14) | (1u << 28) | (1u << 29) | (1u << 30));
    wr16 (0x06a4, 0xffff);                                  /* RXFLTMAP2: all data */
    wr16 (0x06a2, 0x400);                                   /* RXFLTMAP1 */
    wr16 (0x06a0, 0xffff);                                  /* RXFLTMAP0: all mgmt */

    /* init_burst */
    v = rd8 (0x0290);
    v = (v & ~0x3cu) | (1 << 4) | (3 << 2) | 0x02;
    wr8 (0x0290, v);
    wr8 (0x04c7, rd8 (0x04c7) | 0x80);                      /* HT single AMPDU */
    wr16 (0x04ca, 0x0c14);                                  /* MAX_AGGR_NUM */
    wr8 (0x0456, 0x70);                                     /* AMPDU max time */
    wr32 (0x0458, 0xffffffff);                              /* AGGLEN_LMT */
    wr8 (0x060c, 0x18);                                     /* RX_PKT_LIMIT */
    wr8 (0x0512, 0x00);                                     /* PIFS */
    wr8 (0x0420, 0x80);                                     /* FWHW_TXQ_CTRL AMPDU retry */
    wr32 (0x0460, 0x03086666);                              /* FAST_EDCA_CTRL */
    wr8 (0x055c, 0x28);                                     /* USTIME_TSF */
    wr8 (0x0638, 0x28);                                     /* USTIME_EDCA */
    wr8 (REG_RSV_CTRL, rd8 (REG_RSV_CTRL) | 0x60);

    /* init_aggregation: TX agg 6 descs, RX aggregation OFF */
    wr32 (0x0208, (rd32 (0x0208) & ~(0xfu << 4)) | (6 << 4));
    wr8 (0x0228, 6 << 1);
    wr8 (0x010c, rd8 (0x010c) & ~0x04u);
    wr32 (0x0280, rd32 (0x0280) & ~0x80000000u & ~0xff0fu);
    wr8 (0x0290, rd8 (0x0290) & ~0x02u);

    wr32 (0x0800, rd32 (0x0800) | (1u << 24) | (1u << 25));    /* CCK + OFDM on */
    wr8 (0x0040, rd8 (0x0040) & ~0x20u);                    /* GPIO_MUXCFG: IO_SEL_ENBT off */
    wr8 (0x0a0a, 0x83);                                     /* CCK PD threshold */

    /* LC calibration (rtl8188f_phy_lc_calibrate) */
    {
        u32 lstf = rd32 (0x0d00), mode;

        if (lstf & 0x70000000u) {
            wr32 (0x0d00, lstf & ~0x70000000u);
        } else {
            wr8 (0x0522, 0xff);
        }
        mode = rf_read (0x18);
        rf_write (0x18, mode | 0x08000);
        for (n = 0; n < 100; n++) {
            if (!(rf_read (0x18) & 0x08000)) {
                break;
            }
            udelay (10000);
        }
        rf_write (0x18, mode);
        if (lstf & 0x70000000u) {
            wr32 (0x0d00, lstf);
        } else {
            wr8 (0x0522, 0x00);
        }
    }
    return 0;
}

/* rtl8188f_enable_rf + the RX part of rtl8xxxu_start */
static inline void rtl_enable_rf (void) {
    wr8 (0x001f, 0x07);
    wr32 (0x0c04, (rd32 (0x0c04) & ~0xffu) | 0x01 | 0x10);     /* RX A, TX A */
    wr8 (0x0522, 0x00);                                     /* TXPAUSE off */
    wr16 (0x06a4, 0xffff);
    wr16 (0x06a0, 0xffff);
    wr32 (0x0c50, (rd32 (0x0c50) & ~0x7fu) | 0x1e);         /* initial gain */
}

/* ---- TX ----------------------------------------------------------------- */

/* rtl8188f_set_tx_power: indices from eFuse 0x10 (cck_base[6],
 * ht40_base[5], then nibbles a = OFDM diff, b = HT20 diff, signed) */
static inline void rtl_set_tx_power (u32 ch) {
    int group = ch < 3 ? 0 : ch < 6 ? 1 : ch < 9 ? 2 : ch < 12 ? 3 : 4;
    int cck_group = ch == 14 ? 5 : group;
    u32 cck = rtl_efuse[0x10 + cck_group];
    u32 ht40 = rtl_efuse[0x16 + group];
    int diff_a = (signed char) (rtl_efuse[0x1b] << 4) >> 4;
    int diff_b = (signed char) rtl_efuse[0x1b] >> 4;
    u32 ofdm, mcs;

    if (cck > 0x3f) {
        cck = 0x22;
    }
    if (ht40 > 0x3f) {
        ht40 = 0x27;
    }
    wr32 (0x0e08, (rd32 (0x0e08) & 0xffff00ffu) | (cck << 8));
    wr32 (0x086c, (rd32 (0x086c) & 0xffu) | (cck << 8) | (cck << 16) | (cck << 24));
    ofdm = (ht40 + diff_a) & 0xff;
    ofdm |= ofdm << 8 | ofdm << 16 | ofdm << 24;
    wr32 (0x0e00, ofdm);
    wr32 (0x0e04, ofdm);
    mcs = (ht40 + diff_b) & 0xff;
    mcs |= mcs << 8 | mcs << 16 | mcs << 24;
    wr32 (0x0e10, mcs);
    wr32 (0x0e14, mcs);
    wr32 (0x0e18, mcs);
    wr32 (0x0e1c, mcs);
}

/* Our MAC into REG_MACID so the chip ACKs frames sent to us */
static inline void rtl_set_mac (const unsigned char *mac) {
    int i;

    for (i = 0; i < 6; i++) {
        wr8 (0x0610 + i, mac[i]);
    }
}

static unsigned char rtl_txbuf[40 + 2048] __attribute__ ((aligned (32)));

/* Send one management frame (802.11 header included, no FCS) at 1 Mbit/s
 * on the MGNT queue, as rtl8xxxu_tx + fill_txdesc_v2. Returns 0 / -1. */
static inline int rtl_tx_mgmt (const unsigned char *frame, u32 len) {
    unsigned char *d = rtl_txbuf;
    u32 i, w, csum = 0;
    int actual;

    if (len > 2048) {
        return -1;
    }
    for (i = 0; i < 40; i++) {
        d[i] = 0;
    }
    d[0] = len & 0xff;                              /* pkt_size */
    d[1] = len >> 8;
    d[2] = 40;                                      /* pkt_offset */
    d[3] = 0x80 | 0x08 | 0x04;                      /* OWN, FIRST, LAST segment */
    if (frame[4] & 1) {
        d[3] |= 0x01;                               /* broadcast/multicast DA */
    }
    w = 0x12 << 8;                                  /* txdw1: queue MGNT, macid 0 */
    d[4] = w; d[5] = w >> 8; d[6] = w >> 16; d[7] = w >> 24;
    w = 1u << 16;                                   /* txdw2: AGG_BREAK */
    d[8] = w; d[9] = w >> 8; d[10] = w >> 16; d[11] = w >> 24;
    w = 1u << 8;                                    /* txdw3: USE_DRIVER_RATE */
    d[12] = w; d[13] = w >> 8; d[14] = w >> 16; d[15] = w >> 24;
    w = 0 | (6u << 18) | (1u << 17);                /* txdw4: 1M, retry limit 6 */
    d[16] = w; d[17] = w >> 8; d[18] = w >> 16; d[19] = w >> 24;
    w = ((frame[22] | (frame[23] << 8)) >> 4) << 12;    /* txdw9: sequence number */
    d[36] = w; d[37] = w >> 8; d[38] = w >> 16; d[39] = w >> 24;

    for (i = 0; i < 32; i += 2) {                   /* XOR of first 16 words */
        csum ^= d[i] | (d[i + 1] << 8);
    }
    d[28] = csum & 0xff;
    d[29] = csum >> 8;

    for (i = 0; i < len; i++) {
        d[40 + i] = frame[i];
    }
    return ub_bulk (rtl, 0x02, d, 40 + len, &actual, 1000) < 0 ? -1 : 0;
}

/* rtl8188fu_config_channel, 20 MHz, without spur calibration */
static inline void rtl_set_channel (u32 ch) {
    u32 v;

    v = rf_read (0x18);
    rf_write (0x18, (v & ~0x3ffu) | ch);

    wr32 (0x0800, rd32 (0x0800) & ~1u);                     /* 20 MHz */
    wr32 (0x0900, rd32 (0x0900) & ~1u);
    wr32 (0x0800, rd32 (0x0800) | (7u << 8));               /* RXADC clk */
    wr32 (0x0800, (rd32 (0x0800) | (1u << 14) | (1u << 12)) & ~(1u << 13));    /* TXDAC clk */
    wr32 (0x0ce4, rd32 (0x0ce4) & ~(3u << 30));             /* small BW */
    wr32 (0x0ce4, (rd32 (0x0ce4) & ~(1u << 29)) | (1u << 28));
    wr32 (0x0c10, (rd32 (0x0c10) & ~(1u << 29)) | (1u << 28));
    wr32 (0x0954, rd32 (0x0954) & ~(1u << 19));
    wr32 (0x0954, (rd32 (0x0954) & ~(0xfu << 20)) | (1u << 21) | (1u << 20));

    rf_write (0x18, ch | (1u << 10) | (1u << 11));          /* channel + BW 20 */
    rf_write (0x87, 0x00065);                               /* filter BW */
    rf_write (0x1c, 0x0);                                   /* RX_BB2 */
    rf_write (0xdf, 0x00140);                               /* RC corner */
    rf_write (0x1b, 0x01c6c);
}

#endif
