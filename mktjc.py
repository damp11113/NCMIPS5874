# Convert any video into a .TJC file for the box's TJC video player
# (apps/tjcplay): tiled-JPEG video + QOA audio at 48 kHz.
#
# Needs ffmpeg + ffprobe and tjc (tjc/ in this repo) on PATH.
#
#   python mktjc.py myvideo.mp4                  -> MYVIDEO.TJC, 854x480
#   python mktjc.py myvideo.mp4 --size 720       1280x720 (calm content)
#   python mktjc.py myvideo.mp4 --to H:          also copy to H:\VIDEO
#   python mktjc.py clip.mkv --start 1:30 --duration 20 -o CLIP.TJC
#
# The frame rate is read from the video (odd phone rates are snapped to
# the usual ones, rates above --max-fps are halved: 60 -> 30, 50 -> 25),
# ffmpeg is told to output exactly that rate, and the same rate goes to
# tjc, so the sound stays in sync. Pictures that are not 16:9 get black
# bars instead of being stretched. Runs ffmpeg | tjc through Python pipes,
# so it works from any shell (PowerShell 5.1 corrupts binary pipes).
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from fractions import Fraction

SIZES = {"360": (640, 360), "480": (854, 480), "540": (960, 540), "720": (1280, 720)}
COMMON_RATES = [Fraction(24000, 1001), Fraction(24), Fraction(25), Fraction(30000, 1001), Fraction(30),
                Fraction(50), Fraction(60000, 1001), Fraction(60)]


def die(msg):
    print("mktjc: " + msg, file=sys.stderr)
    sys.exit(1)


def need(tool):
    path = shutil.which(tool)
    if not path:
        die(tool + " not found on PATH")
    return path


def probe(path):
    out = subprocess.run([need("ffprobe"), "-v", "error", "-show_streams", "-show_format", "-of", "json", path],
                         capture_output=True, text=True)
    if out.returncode:
        die("ffprobe cannot read " + path + ": " + out.stderr.strip())
    info = json.loads(out.stdout)
    video = next((s for s in info["streams"] if s["codec_type"] == "video"), None)
    audio = next((s for s in info["streams"] if s["codec_type"] == "audio"), None)
    if not video:
        die("no video stream in " + path)
    return video, audio, float(info["format"].get("duration", 0) or 0)


def rate_of(text):
    try:
        r = Fraction(text)
    except (ValueError, ZeroDivisionError):
        return None
    return r if r > 0 else None


def pick_fps(video, override, max_fps):
    if override:
        r = rate_of(override)
        if not r:
            die("bad --fps value: " + override)
        return r
    # avg_frame_rate is the real average (phones record variable rate);
    # r_frame_rate is the container's base rate and can be 90000 or 1000
    r = rate_of(video.get("avg_frame_rate", "")) or rate_of(video.get("r_frame_rate", ""))
    if not r:
        die("cannot read the frame rate, give --fps")
    best = min(COMMON_RATES, key=lambda c: abs(r - c) / c)
    r = best if abs(r - best) / best < Fraction(1, 100) else r.limit_denominator(1001)
    while r > max_fps:
        r /= 2
    return r


def parse_size(text):
    if text in SIZES:
        return SIZES[text]
    try:
        w, h = (int(v) for v in text.lower().split("x"))
    except ValueError:
        die("bad --size, use 360 / 480 / 540 / 720 or WxH")
    if w < 16 or h < 16 or w > 1280 or h > 720:
        die("--size must fit the box's 1280x720 screen")
    return w & ~1, h & ~1


def summary(tjc, out_path, w, h):
    info = subprocess.run([tjc, "info", "-i", out_path], capture_output=True, text=True)
    heavy = frames = 0
    total = 0.0
    for line in (info.stdout + info.stderr).splitlines():
        parts = line.split()
        if len(parts) > 4 and parts[0] == "frame" and "/" in parts[4]:
            d, t = (int(v) for v in parts[4].split("/"))
            frames += 1
            total += d / t
            heavy += d / t > 0.7
    if not frames:
        return
    # rough box budget: ~8-10 Mpix/s of changed area, see the analysis in continue.md
    pix = w * h * total / frames
    print("tiles changed per frame: %.0f%% on average, %d of %d frames above 70%%" %
          (100 * total / frames, heavy, frames))
    if w * h > 600000 and heavy > frames // 10:
        print("note: lots of motion at this size; the box will drop frames. Try --size 480.")
    elif pix > 300000:
        print("note: busy video; if it drops frames on the box, try --threshold 6 --quality 60.")


def main():
    ap = argparse.ArgumentParser(description="Convert a video to .TJC for the box's TJC video player.")
    ap.add_argument("input")
    ap.add_argument("-o", "--output", help="output file (default: input name in capitals, .TJC)")
    ap.add_argument("--size", default="480", help="360, 480 (default), 540, 720 or WxH")
    ap.add_argument("--quality", type=int, default=75, help="1-100, default 75 (lower = smaller file)")
    ap.add_argument("--threshold", type=int, default=3,
                    help="motion threshold, default 3 (higher = less work for the box, may smear fades)")
    ap.add_argument("--fps", help="force a frame rate (24, 25, 29.97, 30000/1001, ...)")
    ap.add_argument("--max-fps", type=float, default=30, help="halve rates above this (default 30)")
    ap.add_argument("--start", help="start time (seconds or h:mm:ss)")
    ap.add_argument("--duration", help="length (seconds or h:mm:ss)")
    ap.add_argument("--no-audio", action="store_true", help="leave out the sound")
    ap.add_argument("--to", help="also copy the result to <this>\\VIDEO, e.g. --to H:")
    a = ap.parse_args()

    ffmpeg, tjc = need("ffmpeg"), need("tjc")
    if not os.path.isfile(a.input):
        die("no such file: " + a.input)
    video, audio, duration = probe(a.input)
    w, h = parse_size(a.size)
    fps = pick_fps(video, a.fps, Fraction(a.max_fps).limit_denominator(1001))
    out_path = a.output or os.path.splitext(os.path.basename(a.input))[0].upper() + ".TJC"
    if not out_path.upper().endswith(".TJC"):
        out_path += ".TJC"
    use_audio = audio is not None and not a.no_audio

    src_fps = rate_of(video.get("avg_frame_rate", "")) or 0
    print("input: %dx%d, %.3f fps, %s, %.1f s" % (int(video["width"]), int(video["height"]), float(src_fps),
                                               "audio " + audio["codec_name"] if audio else "no audio", duration))
    print("output: %s, %dx%d, %s fps (%.3f), quality %d, threshold %d, %s" %
          (out_path, w, h, fps, float(fps), a.quality, a.threshold, "QOA audio 48 kHz" if use_audio else "no audio"))

    trim = []
    if a.start:
        trim += ["-ss", a.start]
    if a.duration:
        trim += ["-t", a.duration]

    with tempfile.TemporaryDirectory() as tmp:
        cmd = [tjc, "encode", "--size", "%dx%d" % (w, h), "--fps", "%d/%d" % (fps.numerator, fps.denominator),
               "--quality", str(a.quality), "--motion-threshold", str(a.threshold), "--refresh-mode", "full", "-o", out_path]
        if use_audio:
            wav = os.path.join(tmp, "audio.wav")
            r = subprocess.run([ffmpeg, "-v", "error", "-y"] + trim + ["-i", a.input, "-vn", "-ar", "48000",
                                "-ac", "2", "-c:a", "pcm_s16le", wav])
            if r.returncode:
                die("ffmpeg could not extract the audio")
            cmd += ["--audio", wav]

        vf = ("scale=%d:%d:force_original_aspect_ratio=decrease:flags=lanczos,pad=%d:%d:-1:-1,setsar=1"
              % (w, h, w, h))
        ff = subprocess.Popen([ffmpeg, "-v", "error", "-stats"] + trim +
                              ["-i", a.input, "-an", "-vf", vf, "-r", "%d/%d" % (fps.numerator, fps.denominator),
                               "-f", "rawvideo", "-pix_fmt", "yuv420p", "-"], stdout=subprocess.PIPE)
        enc = subprocess.Popen(cmd, stdin=ff.stdout)
        ff.stdout.close()
        enc.wait()
        ff.wait()
        if ff.returncode or enc.returncode:
            die("encoding failed (ffmpeg %d, tjc %d)" % (ff.returncode, enc.returncode))

    size = os.path.getsize(out_path)
    print("wrote %s, %.1f MB" % (out_path, size / 1e6))
    summary(tjc, out_path, w, h)

    if a.to:
        dest = os.path.join(a.to if a.to.endswith(("\\", "/")) else a.to + "\\", "VIDEO")
        os.makedirs(dest, exist_ok=True)
        target = os.path.join(dest, os.path.basename(out_path))
        shutil.copyfile(out_path, target)
        print("copied to " + target)


if __name__ == "__main__":
    main()
