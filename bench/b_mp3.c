/* Helix MP3 decoder: decode a whole MP3 held in RAM, time MP3Decode only */
#include <stdlib.h>
#include <string.h>

#include "mp3dec.h"
#include "bench.h"

void *helix_malloc(int size) {
    return malloc(size);
}

void helix_free(void *ptr) {
    free(ptr);
}

/* 1 = decoded something */
int bench_mp3(struct bench_result *r, const unsigned char *mp3, int len) {
    static short pcm[1152 * 2];
    HMP3Decoder dec = MP3InitDecoder();
    unsigned char *p = (unsigned char *) mp3;
    int left = len, i, rate = 0;
    u32 t = 0, h = 0, t0, samples = 0;

    if (!dec) {
        printf("mp3: MP3InitDecoder failed\n");
        return 0;
    }
    while (left > 0) {
        MP3FrameInfo fi;
        int off = MP3FindSyncWord(p, left), err;

        if (off < 0) {
            break;
        }
        p += off;
        left -= off;
        t0 = bench_ticks();
        err = MP3Decode(dec, &p, &left, pcm, 0);
        t += bench_ticks() - t0;
        if (err == ERR_MP3_INDATA_UNDERFLOW || err == ERR_MP3_MAINDATA_UNDERFLOW) {
            if (err == ERR_MP3_INDATA_UNDERFLOW) {
                break;
            }
            continue;
        }
        if (err) {                          /* bad frame: step past this sync */
            p++;
            left--;
            continue;
        }
        MP3GetLastFrameInfo(dec, &fi);
        rate = fi.samprate;
        samples += fi.outputSamps / fi.nChans;
        for (i = 0; i < fi.outputSamps; i++) {
            h = h * 31 + (u32) (unsigned short) pcm[i];
        }
    }
    MP3FreeDecoder(dec);
    r->ticks = t;
    r->audio_ms = rate ? (u32) ((unsigned long long) samples * 1000 / rate) : 0;
    r->sum = h;
    return samples != 0;
}
