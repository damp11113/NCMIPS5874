// tjc - command line encoder/decoder for the Tiled-JPEG Codec (TJC2).
//
//   tjc encode --width W --height H [options] [--audio in.wav] [-i in.yuv] [-o out.tjc]
//   tjc decode [-i in.tjc] [-o out.yuv] [--audio-out out.wav]
//   tjc info   [-i in.tjc]
//
// Input/output default to stdin/stdout, so it sits in an ffmpeg pipe:
//   ffmpeg -i in.mp4 -f rawvideo -pix_fmt yuv420p - | tjc encode --width 320 --height 240 > s.tjc
//   tjc decode < s.tjc | ffplay -f rawvideo -pix_fmt yuv420p -video_size 320x240 -
//
// When the binary is named (or symlinked as) tjc_encode / tjc_decode the
// subcommand can be left out.
//
// Build: g++ -std=c++17 -O2 -pthread -o tjc tjc.cpp

#define TJC_IMPLEMENTATION
#include "tjc.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
    #include <io.h>
    #include <fcntl.h>
#endif

namespace {

void usage() {
    std::fprintf(stderr,
        "Tiled-JPEG Codec (TJC2)\n"
        "\n"
        "usage:\n"
        "  tjc encode --width W --height H [options] [-i in.yuv] [-o out.tjc]\n"
        "  tjc decode [-i in.tjc] [-o out.yuv] [--audio-out out.wav] [-v] [-q]\n"
        "  tjc info   [-i in.tjc] [-v]\n"
        "\n"
        "encode options:\n"
        "  --width, -W N              frame width in pixels (required)\n"
        "  --height, -H N             frame height in pixels (required)\n"
        "  --size WxH                 shorthand for --width/--height\n"
        "  --fps RATE                 frame rate of the input: 30, 29.97, 30000/1001 ... (default 30)\n"
        "  --audio FILE.wav           add an audio track (16-bit PCM WAV, any rate, 1..8 channels)\n"
        "  --tile-size WxH | N        tile size 1..255 (default 16x16; below 8x8 costs more bits, see README)\n"
        "  --motion-threshold K       dirty if mean |diff| per sample > K (default 3, 0 = any change)\n"
        "  --refresh-mode MODE        none | full | rolling (default rolling)\n"
        "  --refresh-param N          full: refresh every N frames, rolling: N tiles per frame (default 30)\n"
        "  --quality Q                1..100 (default 75)\n"
        "  --diff-ref REF             last-coded | recon: what new frames are compared to (default last-coded)\n"
        "  --keyframe-every N         also force a full refresh every N frames (default 0 = off)\n"
        "  --psnr                     report PSNR of the reconstruction vs the source\n"
        "  --threads N                encoder threads, 0 = all cores (default 0)\n"
        "\n"
        "decode options:\n"
        "  --audio-out FILE.wav       write the audio track as a 16-bit PCM WAV file\n"
        "\n"
        "common options:\n"
        "  -i FILE                    input (default stdin, '-' = stdin)\n"
        "  -o FILE                    output (default stdout, '-' = stdout)\n"
        "  -v, --verbose              per-frame log on stderr\n"
        "  -q, --quiet                no summary\n"
        "\n"
        "Input/output frames are raw YUV420P (ffmpeg: -f rawvideo -pix_fmt yuv420p).\n"
        "Make the WAV with: ffmpeg -i in.mp4 -vn -c:a pcm_s16le audio.wav\n");
}

struct Options {
    std::string mode;
    std::string in = "-", out = "-";
    std::string audio_in, audio_out;
    tjc::Config cfg;
    bool have_w = false, have_h = false, have_fps = false;
    uint32_t keyframe_every = 0;
    bool verbose = false, quiet = false, psnr = false;
};

[[noreturn]] void die(const char* fmt, const char* arg = "") {
    std::fprintf(stderr, "tjc: ");
    std::fprintf(stderr, fmt, arg);
    std::fprintf(stderr, "\n");
    std::exit(1);
}

long parse_int(const char* s, const char* what, long lo, long hi) {
    char* end = nullptr;
    long v = std::strtol(s, &end, 10);
    if (!*s || *end || v < lo || v > hi) die("invalid value for %s", what);
    return v;
}

void parse_pair(const char* s, const char* what, long& a, long& b) {
    std::string str(s);
    size_t x = str.find_first_of("xX");
    if (x == std::string::npos) {
        a = b = parse_int(s, what, 1, 65535);
        return;
    }
    a = parse_int(str.substr(0, x).c_str(), what, 1, 65535);
    b = parse_int(str.substr(x + 1).c_str(), what, 1, 65535);
}

// "30", "30000/1001", or a decimal such as "29.97" (NTSC rates map to x000/1001).
void parse_rate(const char* s, uint32_t& num, uint32_t& den) {
    std::string str(s);
    size_t slash = str.find('/');
    if (slash != std::string::npos) {
        num = uint32_t(parse_int(str.substr(0, slash).c_str(), "--fps", 1, 1000000000));
        den = uint32_t(parse_int(str.substr(slash + 1).c_str(), "--fps", 1, 1000000000));
        return;
    }
    char* end = nullptr;
    double v = std::strtod(s, &end);
    if (!*s || *end || !(v > 0) || v > 1000000) die("invalid value for --fps");
    for (double ntsc : {24.0, 30.0, 48.0, 60.0, 120.0, 240.0}) {
        if (std::fabs(v - ntsc * 1000.0 / 1001.0) < 0.006) {
            num = uint32_t(ntsc * 1000);
            den = 1001;
            return;
        }
    }
    if (v == std::floor(v)) {
        num = uint32_t(v);
        den = 1;
    } else {
        num = uint32_t(std::lround(v * 1000));
        den = 1000;
    }
}

Options parse_args(int argc, char** argv) {
    Options o;
    int i = 1;
    std::string prog = argv[0];
    size_t slash = prog.find_last_of("/\\");
    if (slash != std::string::npos) prog = prog.substr(slash + 1);
    if (prog.rfind("tjc_encode", 0) == 0) o.mode = "encode";
    else if (prog.rfind("tjc_decode", 0) == 0) o.mode = "decode";
    else if (prog.rfind("tjc_info", 0) == 0) o.mode = "info";

    if (o.mode.empty()) {
        if (argc < 2) {
            usage();
            std::exit(1);
        }
        o.mode = argv[1];
        i = 2;
        if (o.mode == "-h" || o.mode == "--help" || o.mode == "help") {
            usage();
            std::exit(0);
        }
        if (o.mode != "encode" && o.mode != "decode" && o.mode != "info") die("unknown command '%s'", argv[1]);
    }

    for (; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) die("missing value for %s", argv[i]);
            return argv[++i];
        };
        if (a == "-h" || a == "--help") {
            usage();
            std::exit(0);
        } else if (a == "-i" || a == "--input") {
            o.in = next();
        } else if (a == "-o" || a == "--output") {
            o.out = next();
        } else if (a == "-v" || a == "--verbose") {
            o.verbose = true;
        } else if (a == "-q" || a == "--quiet") {
            o.quiet = true;
        } else if (a == "--fps" || a == "-r") {
            parse_rate(next(), o.cfg.fps_num, o.cfg.fps_den);
            o.have_fps = true;
        } else if (a == "--audio") {
            o.audio_in = next();
        } else if (a == "--audio-out") {
            o.audio_out = next();
        } else if (a == "--width" || a == "-W") {
            o.cfg.width = uint16_t(parse_int(next(), "--width", 1, 65535));
            o.have_w = true;
        } else if (a == "--height" || a == "-H") {
            o.cfg.height = uint16_t(parse_int(next(), "--height", 1, 65535));
            o.have_h = true;
        } else if (a == "--size" || a == "-s" || a == "--video-size") {
            long w, h;
            parse_pair(next(), "--size", w, h);
            o.cfg.width = uint16_t(w);
            o.cfg.height = uint16_t(h);
            o.have_w = o.have_h = true;
        } else if (a == "--tile-size") {
            long w, h;
            parse_pair(next(), "--tile-size", w, h);
            if (w > 255 || h > 255) die("--tile-size must be at most 255x255");
            o.cfg.tile_w = uint8_t(w);
            o.cfg.tile_h = uint8_t(h);
        } else if (a == "--motion-threshold") {
            o.cfg.motion_threshold_k = uint32_t(parse_int(next(), "--motion-threshold", 0, 255));
        } else if (a == "--refresh-mode") {
            std::string m = next();
            if (m == "none") o.cfg.refresh_mode = tjc::RefreshMode::None;
            else if (m == "full" || m == "full-periodic" || m == "periodic") o.cfg.refresh_mode = tjc::RefreshMode::FullPeriodic;
            else if (m == "rolling") o.cfg.refresh_mode = tjc::RefreshMode::Rolling;
            else die("unknown refresh mode '%s' (none | full | rolling)", m.c_str());
        } else if (a == "--refresh-param") {
            o.cfg.refresh_param = uint32_t(parse_int(next(), "--refresh-param", 0, 65535));
        } else if (a == "--quality") {
            o.cfg.quality = uint8_t(parse_int(next(), "--quality", 1, 100));
        } else if (a == "--diff-ref") {
            std::string m = next();
            if (m == "last-coded" || m == "source") o.cfg.diff_reference = tjc::DiffReference::LastCoded;
            else if (m == "recon" || m == "reconstructed") o.cfg.diff_reference = tjc::DiffReference::Reconstructed;
            else die("unknown diff reference '%s' (last-coded | recon)", m.c_str());
        } else if (a == "--keyframe-every") {
            o.keyframe_every = uint32_t(parse_int(next(), "--keyframe-every", 0, 1L << 30));
        } else if (a == "--threads") {
            o.cfg.threads = int(parse_int(next(), "--threads", 0, 256));
        } else if (a == "--psnr") {
            o.psnr = true;
        } else {
            die("unknown option '%s' (see --help)", a.c_str());
        }
    }
    return o;
}

FILE* open_in(const std::string& path) {
    if (path == "-") {
#ifdef _WIN32
        _setmode(_fileno(stdin), _O_BINARY);
#endif
        return stdin;
    }
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) die("cannot open input '%s'", path.c_str());
    return f;
}

FILE* open_out(const std::string& path) {
    if (path == "-") {
#ifdef _WIN32
        _setmode(_fileno(stdout), _O_BINARY);
#endif
        return stdout;
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) die("cannot open output '%s'", path.c_str());
    return f;
}

void write_all(FILE* f, const void* data, size_t n) {
    if (n && std::fwrite(data, 1, n, f) != n) die("write failed");
}

size_t file_read(void* user, void* dst, size_t n) { return std::fread(dst, 1, n, static_cast<FILE*>(user)); }

uint32_t le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint32_t le16(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8; }
void put_le32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
void put_le16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }

// Streaming reader for 16-bit PCM WAV (plain or WAVE_FORMAT_EXTENSIBLE). A data
// size of 0 or 0xFFFFFFFF (what ffmpeg writes to a pipe) means "until EOF".
struct WavReader {
    FILE* f = nullptr;
    int channels = 0;
    uint32_t rate = 0;
    uint64_t left = ~uint64_t(0);  // bytes of sample data left
    std::vector<uint8_t> raw;

    void open(const std::string& path) {
        f = open_in(path);
        uint8_t h[12];
        if (std::fread(h, 1, 12, f) != 12 || std::memcmp(h, "RIFF", 4) || std::memcmp(h + 8, "WAVE", 4))
            die("'%s' is not a WAV file (make one with: ffmpeg -i in.mp4 -vn -c:a pcm_s16le audio.wav)", path.c_str());
        bool have_fmt = false;
        for (;;) {
            uint8_t ck[8];
            if (std::fread(ck, 1, 8, f) != 8) die("'%s': no audio data found", path.c_str());
            uint32_t size = le32(ck + 4);
            if (!std::memcmp(ck, "fmt ", 4)) {
                if (size < 16 || size > 1024) die("'%s': bad fmt chunk", path.c_str());
                std::vector<uint8_t> fmt(size + (size & 1));
                if (std::fread(fmt.data(), 1, fmt.size(), f) != fmt.size()) die("'%s': truncated", path.c_str());
                uint32_t format = le16(&fmt[0]);
                if (format == 0xfffe && size >= 26) format = le16(&fmt[24]);
                channels = int(le16(&fmt[2]));
                rate = le32(&fmt[4]);
                if (format != 1 || le16(&fmt[14]) != 16)
                    die("'%s' must be 16-bit PCM (ffmpeg: -c:a pcm_s16le)", path.c_str());
                if (channels < 1 || channels > 8) die("'%s': 1..8 channels supported", path.c_str());
                have_fmt = true;
            } else if (!std::memcmp(ck, "data", 4)) {
                if (!have_fmt) die("'%s': data before fmt chunk", path.c_str());
                if (size != 0 && size != 0xffffffffu) left = size;
                return;
            } else {
                for (uint64_t skip = uint64_t(size) + (size & 1); skip; --skip)
                    if (std::fgetc(f) == EOF) die("'%s': no audio data found", path.c_str());
            }
        }
    }

    // Reads up to n samples per channel; returns how many were read.
    size_t read(std::vector<int16_t>& dst, size_t n) {
        size_t frame = size_t(channels) * 2;
        uint64_t want = std::min<uint64_t>(uint64_t(n) * frame, left);
        raw.resize(size_t(want));
        size_t got = want ? std::fread(raw.data(), 1, size_t(want), f) : 0;
        size_t samples = got / frame;
        left -= got;
        dst.resize(samples * size_t(channels));
        for (size_t i = 0; i < dst.size(); ++i) dst[i] = int16_t(le16(&raw[2 * i]));
        return samples;
    }

    uint64_t remaining_samples() {
        std::vector<int16_t> tmp;
        uint64_t total = 0;
        while (size_t n = read(tmp, 65536)) total += n;
        return total;
    }
};

struct WavWriter {
    FILE* f = nullptr;
    uint64_t data_bytes = 0;
    std::vector<uint8_t> buf;

    void open(const std::string& path, int channels, uint32_t rate) {
        f = open_out(path);
        uint8_t h[44] = {};
        std::memcpy(h, "RIFF", 4);
        put_le32(h + 4, 0xffffffffu);
        std::memcpy(h + 8, "WAVEfmt ", 8);
        put_le32(h + 16, 16);
        put_le16(h + 20, 1);
        put_le16(h + 22, uint32_t(channels));
        put_le32(h + 24, rate);
        put_le32(h + 28, rate * uint32_t(channels) * 2);
        put_le16(h + 32, uint32_t(channels) * 2);
        put_le16(h + 34, 16);
        std::memcpy(h + 36, "data", 4);
        put_le32(h + 40, 0xffffffffu);
        write_all(f, h, 44);
    }

    void write(const int16_t* s, size_t count) {
        buf.resize(count * 2);
        for (size_t i = 0; i < count; ++i) put_le16(&buf[2 * i], uint32_t(uint16_t(s[i])));
        write_all(f, buf.data(), buf.size());
        data_bytes += buf.size();
    }

    // Fills in the real sizes when the output is seekable; a pipe keeps the
    // "unknown size" values, which players accept.
    void close() {
        if (!f) return;
        if (data_bytes <= 0xffffffffu - 36 && std::fseek(f, 4, SEEK_SET) == 0) {
            uint8_t v[4];
            put_le32(v, uint32_t(36 + data_bytes));
            write_all(f, v, 4);
            if (std::fseek(f, 40, SEEK_SET) == 0) {
                put_le32(v, uint32_t(data_bytes));
                write_all(f, v, 4);
            }
        }
        if (f != stdout) std::fclose(f);
        f = nullptr;
    }
};

double psnr(double sse, double count) {
    if (sse <= 0) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 * count / sse);
}

const char* refresh_name(tjc::RefreshMode m) {
    switch (m) {
        case tjc::RefreshMode::None: return "none";
        case tjc::RefreshMode::FullPeriodic: return "full-periodic";
        case tjc::RefreshMode::Rolling: return "rolling";
    }
    return "?";
}

double fps_of(const tjc::StreamHeader& h) { return double(h.fps_num) / double(h.fps_den); }

void print_header(const tjc::StreamHeader& h, const tjc::Layout& l) {
    std::fprintf(stderr, "stream: TJC%u %ux%u @ %.4g fps (%u/%u), tiles %ux%u (%dx%d = %d), quality %u, refresh %s/%u\n",
                 h.version, h.width, h.height, fps_of(h), h.fps_num, h.fps_den, h.tile_w, h.tile_h, l.cols, l.rows,
                 l.tiles, h.quality, refresh_name(h.refresh_mode), h.refresh_param);
    if (h.audio_codec) std::fprintf(stderr, "audio:  QOA, %u Hz, %u channel(s)\n", h.audio_rate, h.audio_channels);
    else std::fprintf(stderr, "audio:  none\n");
}

void log_frame(const tjc::FrameStats& s, bool audio) {
    std::fprintf(stderr, "frame %6u %c dirty %5u/%-5u (%5.1f%%) %8zu B", s.frame_num, s.force_refresh ? 'I' : 'P',
                 s.dirty_tiles, s.total_tiles, 100.0 * s.dirty_tiles / (s.total_tiles ? s.total_tiles : 1), s.bytes);
    if (audio) std::fprintf(stderr, "  audio %5u smp %6zu B", s.audio_samples, s.audio_bytes);
    std::fprintf(stderr, "\n");
}

struct Totals {
    uint64_t frames = 0, iframes = 0, bytes = 0, dirty = 0, tiles = 0;
    uint64_t audio_samples = 0, audio_bytes = 0, audio_padded = 0;
    void add(const tjc::FrameStats& s) {
        ++frames;
        iframes += s.force_refresh;
        bytes += s.bytes;
        dirty += s.dirty_tiles;
        tiles += s.total_tiles;
        audio_samples += s.audio_samples;
        audio_bytes += s.audio_bytes;
        audio_padded += s.audio_padded;
    }
    void print(const char* what, size_t raw_frame, const tjc::StreamHeader& h, uint64_t stream_bytes) const {
        double fps = fps_of(h);
        double raw = double(raw_frame) * double(frames);
        double secs = double(frames) / fps;
        std::fprintf(stderr,
                     "%s: %llu frames (%llu full refresh), %llu bytes, %.1f B/frame avg, %.1f kbit/s @ %.4g fps\n"
                     "      dirty tiles %.1f%%, compression %.1f:1 vs raw yuv420p\n",
                     what, (unsigned long long)frames, (unsigned long long)iframes, (unsigned long long)stream_bytes,
                     frames ? double(bytes) / double(frames) : 0.0,
                     frames ? double(stream_bytes) * 8.0 / secs / 1000.0 : 0.0, fps,
                     tiles ? 100.0 * double(dirty) / double(tiles) : 0.0,
                     stream_bytes ? raw / double(stream_bytes) : 0.0);
        if (h.audio_codec)
            std::fprintf(stderr, "      audio: %.2f s, %llu bytes, %.1f kbit/s\n", double(audio_samples) / h.audio_rate,
                         (unsigned long long)audio_bytes, secs > 0 ? double(audio_bytes) * 8.0 / secs / 1000.0 : 0.0);
    }
};

int run_encode(Options o) {
    if (!o.have_w || !o.have_h) die("encode needs --width and --height (or --size WxH)");
    WavReader wav;
    if (!o.audio_in.empty()) {
        if (o.audio_in == o.in) die("--audio and -i cannot both be stdin");
        wav.open(o.audio_in);
        o.cfg.audio_channels = uint8_t(wav.channels);
        o.cfg.audio_rate = wav.rate;
        if (!o.have_fps)
            std::fprintf(stderr, "tjc: note: no --fps given, assuming 30; audio stays in sync only if the video is 30 fps\n");
    }
    tjc::Encoder enc;
    if (!enc.init(o.cfg)) die("%s", enc.error());

    FILE* in = open_in(o.in);
    FILE* out = open_out(o.out);
    const size_t fsize = tjc::frame_size(o.cfg.width, o.cfg.height);
    std::vector<uint8_t> frame(fsize), recon(o.psnr ? fsize : 0), buf;
    std::vector<int16_t> pcm;
    buf.reserve(fsize);

    enc.write_stream_header(buf);
    write_all(out, buf.data(), buf.size());
    uint64_t stream_bytes = buf.size();
    const tjc::StreamHeader hdr = enc.stream_header();
    if (o.verbose) print_header(hdr, enc.layout());

    Totals tot;
    double sse[3] = {0, 0, 0};
    const tjc::Layout& l = enc.layout();
    const size_t plane_off[3] = {0, size_t(l.width) * l.height, size_t(l.width) * l.height + size_t(l.cwidth) * l.cheight};
    const size_t plane_len[3] = {size_t(l.width) * l.height, size_t(l.cwidth) * l.cheight, size_t(l.cwidth) * l.cheight};

    for (;;) {
        size_t got = std::fread(frame.data(), 1, fsize, in);
        if (got == 0) break;
        if (got < fsize) {
            std::fprintf(stderr, "tjc: warning: dropped trailing partial frame (%zu of %zu bytes)\n", got, fsize);
            break;
        }
        if (wav.f) {
            size_t need = enc.samples_for_next_frame();
            size_t queued = enc.queued_audio();
            if (need > queued && wav.read(pcm, need - queued)) enc.push_audio(pcm.data(), pcm.size() / size_t(wav.channels));
        }
        if (o.keyframe_every && tot.frames && tot.frames % o.keyframe_every == 0) enc.force_keyframe();
        tjc::FrameStats st;
        buf.clear();
        enc.encode_frame(frame.data(), buf, &st);
        write_all(out, buf.data(), buf.size());
        stream_bytes += buf.size();
        tot.add(st);
        if (o.verbose) log_frame(st, wav.f != nullptr);
        if (o.psnr) {
            enc.copy_recon(recon.data());
            for (int c = 0; c < 3; ++c) {
                const uint8_t* a = frame.data() + plane_off[c];
                const uint8_t* b = recon.data() + plane_off[c];
                double e = 0;
                for (size_t k = 0; k < plane_len[c]; ++k) {
                    double d = double(a[k]) - double(b[k]);
                    e += d * d;
                }
                sse[c] += e;
            }
        }
    }
    std::fflush(out);
    if (wav.f) {
        uint64_t extra = wav.remaining_samples() + enc.queued_audio();
        if (tot.audio_padded)
            std::fprintf(stderr, "tjc: warning: audio ended %.2f s before the video; padded with silence\n",
                         double(tot.audio_padded) / wav.rate);
        if (extra)
            std::fprintf(stderr, "tjc: warning: audio is %.2f s longer than the video; the extra part was dropped\n",
                         double(extra) / wav.rate);
        if (wav.f != stdin) std::fclose(wav.f);
    }
    if (!o.quiet) {
        tot.print("encoded", fsize, hdr, stream_bytes);
        if (o.psnr && tot.frames) {
            double fr = double(tot.frames);
            double n[3] = {double(plane_len[0]) * fr, double(plane_len[1]) * fr, double(plane_len[2]) * fr};
            std::fprintf(stderr, "      PSNR Y %.2f dB, Cb %.2f dB, Cr %.2f dB, all %.2f dB\n", psnr(sse[0], n[0]),
                         psnr(sse[1], n[1]), psnr(sse[2], n[2]), psnr(sse[0] + sse[1] + sse[2], n[0] + n[1] + n[2]));
        }
    }
    if (in != stdin) std::fclose(in);
    if (out != stdout) std::fclose(out);
    return 0;
}

int run_decode(const Options& o, bool write_frames) {
    FILE* in = open_in(o.in);
    tjc::Decoder dec;
    tjc::Status st = dec.read_header(file_read, in);
    if (st == tjc::Status::EndOfStream) die("empty input");
    if (st != tjc::Status::Ok) die("%s", tjc::status_string(st));
    const tjc::StreamHeader& h = dec.header();
    if (o.have_fps) std::fprintf(stderr, "tjc: note: --fps is ignored when decoding; the stream says %u/%u\n", h.fps_num, h.fps_den);

    WavWriter wav;
    if (!o.audio_out.empty()) {
        if (!h.audio_codec) std::fprintf(stderr, "tjc: warning: the stream has no audio; --audio-out ignored\n");
        else if (write_frames && o.audio_out == o.out) die("--audio-out and -o cannot both be stdout");
        else wav.open(o.audio_out, h.audio_channels, h.audio_rate);
    }
    FILE* out = write_frames ? open_out(o.out) : nullptr;
    if (o.verbose || !write_frames) print_header(h, dec.layout());
    if (write_frames && !o.quiet)
        std::fprintf(stderr, "output: rawvideo yuv420p %ux%u @ %u/%u fps  (ffplay -f rawvideo -pix_fmt yuv420p -video_size %ux%u -framerate %u/%u -)\n",
                     h.width, h.height, h.fps_num, h.fps_den, h.width, h.height, h.fps_num, h.fps_den);

    const size_t fsize = tjc::frame_size(h.width, h.height);
    std::vector<uint8_t> frame(write_frames ? fsize : 0);
    Totals tot;
    uint64_t stream_bytes = h.version == 1 ? tjc::kStreamHeaderSizeV1 : tjc::kStreamHeaderSize;
    int rc = 0;
    for (;;) {
        tjc::FrameStats fs;
        st = dec.decode_frame(file_read, in, &fs);
        if (st == tjc::Status::EndOfStream) break;
        if (st != tjc::Status::Ok) {
            std::fprintf(stderr, "tjc: frame %llu: %s\n", (unsigned long long)tot.frames, tjc::status_string(st));
            rc = 1;
            break;
        }
        tot.add(fs);
        stream_bytes += fs.bytes;
        if (o.verbose || !write_frames) log_frame(fs, h.audio_codec != 0);
        if (write_frames) {
            dec.copy_frame(frame.data());
            write_all(out, frame.data(), fsize);
        }
        if (wav.f) wav.write(dec.audio(), dec.audio_samples() * h.audio_channels);
    }
    if (out) std::fflush(out);
    wav.close();
    if (!o.quiet) tot.print(write_frames ? "decoded" : "stream", fsize, h, stream_bytes);
    if (in != stdin) std::fclose(in);
    if (out && out != stdout) std::fclose(out);
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    Options o = parse_args(argc, argv);
    if (o.mode == "encode") return run_encode(o);
    if (o.mode == "decode") return run_decode(o, true);
    return run_decode(o, false);
}
