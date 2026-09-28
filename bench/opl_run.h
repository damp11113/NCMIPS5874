/* Included by b_opl.c (current sdk/opl.c) and b_opl_ref.c (the committed
 * copy in ref_opl.c): all 18 channels playing a sustained FM note
 * (feedback modulator, waveforms on), 1 s at 48 kHz in 32-sample blocks.
 * Returns the CP0 ticks spent in opl_render (). */
static u32 OPL_RUN(int *outl, int *outr) {
    static const int mod_off[9] = { 0, 1, 2, 8, 9, 10, 16, 17, 18 };
    static int left[32], right[32], pan_l[OPL_CHANNELS], pan_r[OPL_CHANNELS];
    u32 t = 0, t0;
    int c, b;

    opl_init(48000);
    opl_write(0x01, 0x20);                          /* waveform select on */
    for (c = 0; c < OPL_CHANNELS; c++) {
        int bank = c < 9 ? 0 : 0x100, k = c % 9, m = bank + mod_off[k], fnum = 300 + c * 23;

        opl_write(m + 0x20, 0x21);                  /* sustain, mult 1 */
        opl_write(m + 0x23, 0x21);
        opl_write(m + 0x40, 0x10);                  /* modulator TL */
        opl_write(m + 0x43, 0x00);
        opl_write(m + 0x60, 0xf4);                  /* fast attack */
        opl_write(m + 0x63, 0xf4);
        opl_write(m + 0x80, 0x27);                  /* sustain level, release */
        opl_write(m + 0x83, 0x27);
        opl_write(m + 0xe0, c & 3);
        opl_write(m + 0xe3, (c >> 2) & 3);
        opl_write(bank + 0xc0 + k, c & 1 ? 0x0e : 0x07);   /* FM + fb 7 / additive + fb 3 */
        opl_write(bank + 0xa0 + k, fnum & 0xff);
        opl_write(bank + 0xb0 + k, 0x20 | (4 << 2) | (fnum >> 8));
        pan_l[c] = 256 - c * 8;
        pan_r[c] = 120 + c * 8;
    }
    for (b = 0; b < 48000 / 32; b++) {
        memset(left, 0, sizeof(left));
        memset(right, 0, sizeof(right));
        t0 = bench_ticks();
        opl_render(left, right, 32, pan_l, pan_r);
        t += bench_ticks() - t0;
        memcpy(outl + b * 32, left, sizeof(left));
        memcpy(outr + b * 32, right, sizeof(right));
    }
    return t;
}
