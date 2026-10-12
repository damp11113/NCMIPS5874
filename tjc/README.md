# TJC — Tiled-JPEG Codec

A small intra-only video codec for low-power decoders (written with a single-core
MIPS 24KEc in mind). Each frame is split into tiles; only tiles that changed (plus
tiles picked by a refresh policy) are sent, each as a tiny baseline-JPEG-style blob.
The decoder keeps a persistent framebuffer and patches it. An optional audio track
is coded with [QOA](https://qoaformat.org) and interleaved per video frame, so audio
and video stay in sync by construction.

| File          | What                                                              |
|---------------|-------------------------------------------------------------------|
| `tjc.h`       | single-header library: encoder + decoder (stb-style)              |
| `tjc.cpp`     | `tjc` command line tool: `encode`, `decode`, `info`               |
| `tjc_test.cpp`| unit tests (bitstream, DCT, Huffman, bitmap, SAD, refresh, QOA) + end-to-end |

## Build

```bash
g++ -std=c++17 -O2 -pthread -o tjc tjc.cpp
g++ -std=c++17 -O2 -pthread -o tjc_test tjc_test.cpp && ./tjc_test
```

or `cmake -B build && cmake --build build && ./build/tjc_test`.

Optional: `ln -s tjc tjc_encode && ln -s tjc tjc_decode` lets you call the tool
without the subcommand.

## Windows

Build with any of these (output: `tjc.exe`):

```bat
:: Visual Studio: "x64 Native Tools Command Prompt for VS"
cl /std:c++17 /O2 /EHsc tjc.cpp

:: MinGW-w64 / MSYS2
g++ -std=c++17 -O2 -static -o tjc.exe tjc.cpp

:: CMake (either toolchain)
cmake -B build && cmake --build build --config Release
```

Get ffmpeg with `winget install Gyan.FFmpeg` (or a build from ffmpeg.org).

**Run the pipes from `cmd.exe`, not Windows PowerShell 5.1.** PowerShell 5.1 sends
pipe data through as text and corrupts binary video. It also has no `<` redirect.
PowerShell 7.4+ passes bytes between programs unchanged, but still has no `<`, so
use `-i`/`-o` there.

```bat
:: cmd.exe
ffmpeg -i input.mp4 -f rawvideo -pix_fmt yuv420p -s 320x240 - | tjc encode --size 320x240 > stream.tjc
tjc decode < stream.tjc | ffplay -f rawvideo -pix_fmt yuv420p -video_size 320x240 -
```

```powershell
# PowerShell (7.4+ for the pipe; the -i/-o form works in any version)
ffmpeg -i input.mp4 -f rawvideo -pix_fmt yuv420p -s 320x240 raw.yuv
.\tjc encode --size 320x240 -i raw.yuv -o stream.tjc
.\tjc decode -i stream.tjc -o out.yuv
ffplay -f rawvideo -pix_fmt yuv420p -video_size 320x240 out.yuv
```

With audio (cmd.exe; check the frame rate with `ffprobe input.mp4` first):

```bat
ffmpeg -i input.mp4 -vn -c:a pcm_s16le audio.wav
ffmpeg -i input.mp4 -f rawvideo -pix_fmt yuv420p - | tjc encode --size 1920x1080 --fps 29.97 --audio audio.wav -o stream.tjc
tjc decode -i stream.tjc -o out.yuv --audio-out out.wav
ffmpeg -f rawvideo -pix_fmt yuv420p -s 1920x1080 -framerate 30000/1001 -i out.yuv -i out.wav -c:v libx264 -c:a aac out.mp4
```

`copy tjc.exe tjc_encode.exe` (and `tjc_decode.exe`) gives the subcommand-free names,
since Windows has no handy symlinks.

## Using the tool with ffmpeg

Frames in and out are raw YUV420P, so the tool sits in an ffmpeg pipe.

```bash
# Encode
ffmpeg -i input.mp4 -f rawvideo -pix_fmt yuv420p -s 320x240 - \
  | ./tjc encode --width 320 --height 240 --tile-size 16x16 --motion-threshold 3 \
      --refresh-mode rolling --refresh-param 30 --quality 75 > stream.tjc

# Preview
./tjc decode < stream.tjc | ffplay -f rawvideo -pix_fmt yuv420p -video_size 320x240 -

# Back to a normal file
./tjc decode < stream.tjc \
  | ffmpeg -f rawvideo -pix_fmt yuv420p -s 320x240 -i - -c:v libx264 out.mp4

# Inspect a stream (header + per-frame dirty tiles and sizes)
./tjc info -i stream.tjc
```

The decoder reads width/height/tile size/frame rate from the stream; it needs no
options. Add `-v` for a per-frame log on stderr and `--psnr` (encoder) for quality
numbers.

### Audio

```bash
# 1. Pull the audio out as 16-bit PCM WAV (any rate, 1..8 channels)
ffmpeg -i input.mp4 -vn -c:a pcm_s16le audio.wav

# 2. Encode video + audio. --fps must match the video for the audio to stay in sync.
ffmpeg -i input.mp4 -f rawvideo -pix_fmt yuv420p - \
  | ./tjc encode --size 1920x1080 --fps 29.97 --audio audio.wav > stream.tjc

# 3. Decode both
./tjc decode -i stream.tjc -o out.yuv --audio-out out.wav

# 4. Watch it, or put it back into an mp4
ffmpeg -f rawvideo -pix_fmt yuv420p -s 1920x1080 -framerate 30000/1001 -i out.yuv \
  -i out.wav -c:v libx264 -c:a aac out.mp4
```

- `--fps` accepts `30`, `25`, `29.97`, `23.976`, `59.94` or an exact `30000/1001`.
  NTSC decimals are mapped to their exact `x000/1001` rates.
- Each video frame carries the audio that plays during it. Frame *n* ends at sample
  `floor((n+1) * rate * fps_den / fps_num)`, so 48 kHz at 29.97 fps alternates
  between 1601 and 1602 samples per frame and never drifts.
- If the audio is shorter than the video it is padded with silence; if it is longer
  the rest is dropped. The tool warns in both cases.
- To resample for the target (for example a 22.05 kHz mono DAC), let ffmpeg do it in
  step 1: `ffmpeg -i input.mp4 -vn -ar 22050 -ac 1 -c:a pcm_s16le audio.wav`.

QOA uses 3.2 bits per sample: about 280 kbit/s for 44.1 kHz stereo and 320 kbit/s
for 48 kHz stereo. It's integer-only (a 4-tap sign-sign LMS predictor, about 20
integer ops per sample to decode), so it suits a CPU without an FPU. The QOA code in
`tjc.h` is a port of the MIT-licensed [reference](https://github.com/phoboslab/qoa).
It is bit-compatible with it: the same input gives the same bytes, and the frames
inside a `.tjc` are standard QOA frames.

### Encoder options

| Option                   | Default      | Meaning |
|--------------------------|--------------|---------|
| `--width` / `--height` / `--size WxH` | required | frame size |
| `--fps RATE`             | `30`         | frame rate of the input, stored in the stream |
| `--audio FILE.wav`       | none         | add a QOA audio track from a 16-bit PCM WAV |
| `--tile-size WxH` or `N` | `16x16`      | 1..255 each; `w*h` at most 12544 (e.g. 64x64, 128x64, 112x112). See "Choosing a tile size" |
| `--motion-threshold K`   | `3`          | tile is dirty when any 8x8 block's mean abs diff per sample is > K; `0` = any change |
| `--refresh-mode`         | `rolling`    | `none`, `full` (full refresh every N frames), `rolling` (N tiles per frame, round robin) |
| `--refresh-param N`      | `30`         | N for the refresh mode |
| `--quality Q`            | `75`         | 1..100, libjpeg-style scaling of the standard quant tables |
| `--diff-ref`             | `last-coded` | what the new frame is compared to: `last-coded` source pixels or `recon` (decoded pixels) |
| `--keyframe-every N`     | `0`          | additionally force a full refresh every N frames |
| `--threads N`            | `0`          | encoder threads, `0` = all cores. The output is identical for any value |

### Choosing a tile size

Smaller tiles skip more unchanged area but cost more per tile: 2 bytes of `tile_len`,
one bitmap bit, and at least one 8x8 block per component even when the tile covers
only a few pixels. Below 8x8 the overhead outweighs the savings fast. Here are the
numbers for 1920x1080 `testsrc2`, q75, K=5, 4 threads:

| Tile  | Bytes/frame | Dirty | vs raw | Encode speed |
|-------|-------------|-------|--------|--------------|
| 32x32 | 50.6 KB     | 17.0% | 61:1   | 118 fps |
| 16x16 | 50.7 KB     | 11.9% | 61:1   | 140 fps |
| 8x8   | 79.8 KB     |  9.3% | 39:1   | 111 fps |
| 4x4   | 201 KB      |  7.9% | 15:1   | 52 fps  |
| 2x2   | 468 KB      |  7.1% | 6.6:1  | 22 fps  |
| 1x1   | 909 KB      |  6.4% | 3.4:1  | 13 fps  |

A 1x1 tile turns each pixel into three 8x8 blocks plus a length field, which is
more bytes than the raw pixel. A full refresh at 1x1 is bigger than the raw frame.
Use 16x16 or 8x8 unless you have a specific reason not to.

### Speed

- Build optimized: `/O2` (MSVC) or `-O2`/`-O3` (gcc/clang). An unoptimized build
  is about 3.5x slower.
- The encoder uses every core by default (`--threads`). Tiles are split into
  contiguous runs and joined in order, so the stream doesn't depend on the thread
  count. The decoder stays single-threaded for the embedded target.
- Worst case (every tile changes every frame) at 1080p: about 30 fps on 1 thread,
  about 60 fps on 4 threads.

With `--diff-ref recon` at low quality the quantization error alone can exceed a
small K and keep static tiles dirty; `last-coded` doesn't have that problem and still
catches slow drift, because the difference is measured from what was last sent.

## Using the library

```cpp
#define TJC_IMPLEMENTATION   // in exactly one .cpp
#include "tjc.h"

// Encoder
tjc::Config cfg;
cfg.width = 320; cfg.height = 240;
cfg.tile_w = cfg.tile_h = 16;
cfg.fps_num = 30000; cfg.fps_den = 1001;
cfg.audio_channels = 2; cfg.audio_rate = 48000;  // leave 0 for no audio
tjc::Encoder enc;
if (!enc.init(cfg)) puts(enc.error());
std::vector<uint8_t> out;
enc.write_stream_header(out);
enc.push_audio(pcm, enc.samples_for_next_frame());  // interleaved int16; push ahead is fine
enc.encode_frame(yuv420p_frame, out);               // appends one frame record (video + audio)

// Decoder (pull-style input callback; tjc::MemoryReader for buffers)
size_t rd(void* f, void* dst, size_t n) { return fread(dst, 1, n, (FILE*)f); }
tjc::Decoder dec;
dec.read_header(rd, stdin);
while (dec.decode_frame(rd, stdin) == tjc::Status::Ok) {
    dec.copy_frame(frame);                     // or use dec.plane(c) / dec.stride(c)
    play(dec.audio(), dec.audio_samples());    // this frame's audio, interleaved
}
```

Compile-time switches (define before the implementation include):

- `TJC_NO_ENCODER`: decoder only (video + audio). About 24 KB of code on x86-64 at `-O2`.
- `TJC_STREAM_BIG_ENDIAN 1`: big-endian multi-byte stream fields.
- `TJC_MAX_PIXELS n`: largest `width*height` the decoder will accept.
- `TJC_NO_THREADS`: single-threaded encoder with no `<thread>` dependency.

The decode path is integer-only (IJG "islow" DCT) and deterministic: decoder output
is bit-identical to the encoder's internal reconstruction on every platform, so the
two framebuffers never drift apart. Decoder RAM is the padded frame (1.5 bytes per
pixel), one tile payload buffer, and one frame's worth of audio samples.

## Stream format (TJC2)

Multi-byte fields are little-endian (see `TJC_STREAM_BIG_ENDIAN`), except inside
the QOA frames, which keep QOA's own big-endian layout.

```
Stream header, 34 bytes
  magic "TJC2" (4) | width (2) | height (2) | tile_w (1) | tile_h (1)
  chroma_format (1, 0 = 4:2:0) | refresh_mode (1) | refresh_param (2)
  quality (1) | reserved (3, zero)
  fps_num (4) | fps_den (4)
  audio_codec (1, 0 = none, 1 = QOA) | audio_channels (1) | audio_rate (4)
  reserved (2, zero)

Frame
  frame_num (4) | force_refresh (1)
  dirty_bitmap, ceil(tiles/8) bytes, only if force_refresh == 0
      tile i (raster order) = bit (i & 7) of byte (i >> 3)
  per dirty tile: tile_len (2) | payload (tile_len)
  if audio_codec != 0: audio_len (4) | QOA frames (audio_len bytes)
      standard QOA frames of up to 5120 samples per channel, holding the samples
      that play during this video frame

Tile payload
  Huffman-coded 8x8 blocks (standard JPEG Annex K tables), MSB first, zero-padded,
  no 0xFF stuffing. Order: Y blocks, then Cb, then Cr, raster within the tile.
  The DC predictor resets per component per tile, so each tile decodes on its own.
```

TJC1 streams (the first 18 header bytes with magic `TJC1`, no frame rate, no audio)
still decode; they are treated as 30 fps.

Changes from the original plan:

- The first reserved header byte carries `quality`, so the decoder can rebuild the
  same quant tables (the tables themselves stay baked in).
- Tiles can be any size from 1 to 255. Partial 8x8 blocks are padded by edge
  replication, and each 4:2:0 chroma sample belongs to the tile holding its top-left
  luma pixel, so tiles never overlap (see the comment at the top of `tjc.h`).
- Frame sizes don't have to be tile multiples (or even): the codec pads internally
  by edge replication and crops on output.
- The motion threshold applies per 8x8 block instead of averaging over the whole
  tile, so a small object moving inside a large tile is still detected.
- Audio, a v1 non-goal, is now supported (QOA, per-frame interleave), along with a
  frame rate in the stream (TJC2).
