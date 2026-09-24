# Convert a black-and-white video (Bad Apple!!) into one .bav file for
# badapple.c on the box: 1-bit frames, delta + run-length coded, plus the
# soundtrack as MP3 (48 kHz, so the box needs no resampling).
#
# Usage (Windows Python with numpy + opencv, ffmpeg with libmp3lame on PATH):
#   python mkbadapple.py badapple.mp4 badapple.bav [threshold]
#
# File layout (little endian):
#   0x00 "BAV1"
#   0x04 width, 0x08 height, 0x0c fps, 0x10 frame count
#   0x14 audio offset, 0x18 audio length (MP3 bytes)
#   0x1c video offset, 0x20 video length
#   header padded to 0x200, audio and video 512-byte (sector) aligned,
#   so the box can stream them from USB with whole-sector reads
#
# Video stream, per frame: varint run count, then that many varint run
# lengths in raster order, alternating "unchanged" / "toggled" pixels
# (starting with unchanged, may be 0). Pixels after the last run are
# unchanged. Frame 0 is coded against an all-black frame.
# Varint = LEB128 (7 bits per byte, low first, bit 7 = more).
import os
import struct
import subprocess
import sys
import tempfile

import cv2
import numpy as np


def varint(v, out):
    while v >= 0x80:
        out.append((v & 0x7f) | 0x80)
        v >>= 7
    out.append(v)


def encode_frame(diff, out):
    """diff: flat uint8 array, 1 = pixel toggles"""
    runs = []
    if diff.any():
        # boundaries where the value changes
        edges = np.flatnonzero(np.diff(diff)) + 1
        bounds = np.concatenate(([0], edges, [diff.size]))
        lengths = np.diff(bounds)
        if diff[0] == 1:
            runs.append(0)              # empty leading unchanged run
        runs.extend(int(x) for x in lengths)
        if len(runs) % 2 == 1:
            runs.pop()                  # trailing unchanged run is implied
    varint(len(runs), out)
    for r in runs:
        varint(r, out)


def main():
    if len(sys.argv) < 3:
        print(__doc__ or "usage: mkbadapple.py in.mp4 out.bav [threshold]")
        sys.exit(1)
    src, dst = sys.argv[1], sys.argv[2]
    threshold = int(sys.argv[3]) if len(sys.argv) > 3 else 128

    cap = cv2.VideoCapture(src)
    fps = cap.get(cv2.CAP_PROP_FPS)
    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    print(f"video: {w}x{h} @ {fps:.3f} fps")

    video = bytearray()
    prev = np.zeros(w * h, dtype=np.uint8)
    n = 0
    while True:
        ok, frame = cap.read()
        if not ok:
            break
        grey = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        cur = (grey.reshape(-1) >= threshold).astype(np.uint8)
        encode_frame(cur ^ prev, video)
        prev = cur
        n += 1
        if n % 500 == 0:
            print(f"  frame {n}, {len(video)} bytes")
    cap.release()
    print(f"frames: {n}, video stream {len(video)} bytes ({len(video) / n:.0f} B/frame)")

    with tempfile.TemporaryDirectory() as tmp:
        mp3 = os.path.join(tmp, "audio.mp3")
        subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", src,
                        "-vn", "-ac", "2", "-ar", "48000", "-c:a", "libmp3lame",
                        "-b:a", "160k", "-id3v2_version", "0", "-write_xing", "0", mp3],
                       check=True)
        audio = open(mp3, "rb").read()
    print(f"audio: {len(audio)} bytes MP3")

    def align512(x):
        return (x + 511) & ~511

    audio_off = 0x200
    video_off = align512(audio_off + len(audio))
    hdr = struct.pack("<4s8I", b"BAV1", w, h, int(round(fps)), n,
                      audio_off, len(audio), video_off, len(video))
    blob = bytearray(hdr.ljust(audio_off, b"\0"))
    blob += audio
    blob += b"\0" * (video_off - len(blob))
    blob += video
    with open(dst, "wb") as f:
        f.write(blob)
    print(f"wrote {dst}: {len(blob)} bytes (0x{len(blob):x})")


if __name__ == "__main__":
    main()
