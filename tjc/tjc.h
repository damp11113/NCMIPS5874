// tjc.h - Tiled-JPEG Codec (stream format "TJC2"), single-header C++17 library.
//
// A frame is split into fixed-size tiles. Each frame only carries the tiles that
// changed (plus tiles picked by the refresh policy); every carried tile is coded as
// a small baseline-JPEG-style blob (integer DCT, static quant tables, static JPEG
// Huffman tables). The decoder keeps a persistent framebuffer and patches it.
// An optional audio track is interleaved per video frame, coded with QOA ("Quite OK
// Audio", integer-only; see the QOA section below for its license notice).
//
// Usage: in exactly ONE .cpp file
//     #define TJC_IMPLEMENTATION
//     #include "tjc.h"
// and include "tjc.h" normally everywhere else.
//
// Compile-time options (define before including in the implementation file):
//     TJC_STREAM_BIG_ENDIAN 1  write/read multi-byte stream fields big-endian (default 0 = little)
//     TJC_NO_ENCODER           leave out the encoder (decode-only embedded builds)
//     TJC_MAX_PIXELS n         largest width*height the decoder accepts (default 1<<26)
//     TJC_NO_THREADS           single-threaded encoder (no <thread> dependency)
//
// The decode path is integer-only and deterministic: the same stream gives
// bit-identical output on every platform, and the encoder's internal
// reconstruction is bit-identical to what the decoder produces. The same holds
// for audio.
//
// ---------------------------------------------------------------------------
// Stream format (all multi-byte fields little-endian unless TJC_STREAM_BIG_ENDIAN)
//
//   Stream header, once. "TJC2" streams have 34 bytes; "TJC1" streams (no frame
//   rate, no audio) have only the first 18 and are still decoded.
//     magic         4  "TJC2" (or "TJC1")
//     width         2  pixels (luma)
//     height        2  pixels (luma)
//     tile_w        1  1..255 (see "Tiles" below)
//     tile_h        1  1..255
//     chroma_format 1  0 = 4:2:0 (only value accepted in v1)
//     refresh_mode  1  0 = NONE, 1 = FULL_PERIODIC, 2 = ROLLING (informational)
//     refresh_param 2  N for FULL_PERIODIC, tiles per frame for ROLLING
//     quality       1  1..100, selects the quant tables (was reserved byte 0)
//     reserved      3  zero
//     -- TJC2 only --
//     fps_num       4  frame rate numerator   (e.g. 30000)
//     fps_den       4  frame rate denominator (e.g. 1001)
//     audio_codec   1  0 = none, 1 = QOA
//     audio_chans   1  1..8
//     audio_rate    4  sample rate in Hz, 1..16777215
//     reserved      2  zero
//
//   Per frame:
//     frame_num     4  uint32
//     force_refresh 1  0 or 1; 1 = every tile follows and no bitmap is sent
//     dirty_bitmap  ceil(tiles/8) bytes, only when force_refresh == 0.
//                   Tile i (raster order) is bit (i & 7) of byte (i >> 3), LSB first.
//     per dirty tile, raster order:
//       tile_len    2  payload size in bytes (1..65535)
//       payload     tile_len bytes
//     audio, only when audio_codec != 0:
//       audio_len   4  bytes of audio data that follow (may be 0)
//       audio data  standard QOA frames (big-endian, as in the QOA spec), each
//                   holding up to 5120 samples per channel. Together they carry
//                   the samples that play during this video frame: frame n ends at
//                   sample floor((n+1) * audio_rate * fps_den / fps_num).
//
//   Tile payload: Huffman-coded 8x8 blocks, MSB-first bit packing, zero-padded to a
//   byte, no marker byte stuffing. Block order: all Y blocks of the tile (raster),
//   then all Cb blocks, then all Cr blocks. The DC predictor resets to 0 at the start
//   of each component of each tile, so every tile decodes on its own.
//
// Tiles: the codec works on an internal frame padded up to whole tiles (edge pixels
// replicated) and crops on output, so frame sizes need not be tile multiples.
//   Luma: tile (tx, ty) covers x in [tx*tile_w, (tx+1)*tile_w), same for y.
//   Chroma: a 4:2:0 sample belongs to the tile holding its top-left luma pixel, so
//     the tile covers chroma x in [(x0+1)/2, (x1+1)/2) for luma range [x0, x1).
//     With odd tile sizes this can be empty (e.g. odd columns of 1x1 tiles).
//   A tile is covered by ceil(w/8) x ceil(h/8) blocks per component; the last
//   row/column of blocks may be partial: the encoder pads it by replicating the
//   tile's edge pixels and the decoder keeps only the covered part.
// ---------------------------------------------------------------------------

#ifndef TJC_H_INCLUDED
#define TJC_H_INCLUDED

#include <cstddef>
#include <cstdint>
#include <vector>

#ifndef TJC_STREAM_BIG_ENDIAN
#define TJC_STREAM_BIG_ENDIAN 0
#endif

#ifndef TJC_MAX_PIXELS
#define TJC_MAX_PIXELS (1u << 26)
#endif

namespace tjc {

constexpr int kStreamHeaderSize = 34;    // TJC2
constexpr int kStreamHeaderSizeV1 = 18;  // TJC1
constexpr int kFrameHeaderSize = 5;  // without the bitmap
constexpr int kMaxTilePayload = 65535;

enum class RefreshMode : uint8_t { None = 0, FullPeriodic = 1, Rolling = 2 };

// What the encoder compares the incoming frame against to decide if a tile is dirty.
//   LastCoded:     the source pixels as they were when the tile was last sent. Slow
//                  drift still accumulates until it crosses the threshold, and static
//                  content is never re-sent just because of quantization noise.
//   Reconstructed: the decoded (lossy) pixels the decoder is showing. Catches every
//                  deviation, but at low quality the quantization error alone can
//                  exceed a small threshold and keep static tiles dirty.
enum class DiffReference : uint8_t { LastCoded = 0, Reconstructed = 1 };

struct Config {
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t tile_w = 16;
    uint8_t tile_h = 16;

    // A tile is dirty when any of its 8x8 blocks (Y, Cb or Cr) has SAD > 64 * K,
    // i.e. the mean absolute difference per sample in that block exceeds K. Judging
    // per block keeps the meaning of K independent of the tile size: a small moving
    // object inside a big tile is not averaged away. K = 0 means any change.
    uint32_t motion_threshold_k = 3;

    RefreshMode refresh_mode = RefreshMode::Rolling;
    uint32_t refresh_param = 30;  // N for FullPeriodic, tiles per frame for Rolling

    uint8_t quality = 75;  // 1..100, libjpeg-style scaling of the standard tables

    DiffReference diff_reference = DiffReference::LastCoded;

    // Encoder worker threads, 0 = one per hardware thread. The output does not
    // depend on this value. Ignored with TJC_NO_THREADS.
    int threads = 0;

    // Frame rate, stored in the stream; also sets how many audio samples go with
    // each frame.
    uint32_t fps_num = 30;
    uint32_t fps_den = 1;

    // Audio track: 0 channels = no audio. Feed samples with Encoder::push_audio().
    uint8_t audio_channels = 0;
    uint32_t audio_rate = 0;
};

struct StreamHeader {
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t tile_w = 0;
    uint8_t tile_h = 0;
    uint8_t chroma_format = 0;
    RefreshMode refresh_mode = RefreshMode::None;
    uint16_t refresh_param = 0;
    uint8_t quality = 0;
    uint8_t version = 2;      // 1 = TJC1 (fields below absent: 30 fps, no audio)
    uint32_t fps_num = 30;
    uint32_t fps_den = 1;
    uint8_t audio_codec = 0;  // 0 = none, 1 = QOA
    uint8_t audio_channels = 0;
    uint32_t audio_rate = 0;
};

struct FrameStats {
    uint32_t frame_num = 0;
    bool force_refresh = false;
    uint32_t dirty_tiles = 0;
    uint32_t total_tiles = 0;
    size_t bytes = 0;            // encoded size of the frame record, audio included
    uint32_t audio_samples = 0;  // samples per channel carried by this frame
    size_t audio_bytes = 0;      // audio_len
    uint32_t audio_padded = 0;   // encoder: samples of silence added because push_audio() ran short
};

enum class Status {
    Ok = 0,
    EndOfStream,     // clean end of input at a frame boundary
    Truncated,       // input ended in the middle of a record
    BadHeader,       // stream header invalid or unsupported
    Corrupt,         // malformed frame or tile data
    OutOfMemory,
    NotInitialized,  // decode_frame() before read_header()
};

const char* status_string(Status s);

// Pull-style input. Return the number of bytes copied into dst (0 = end of input).
// Short reads are fine; the decoder keeps calling until it has what it needs.
typedef size_t (*ReadFn)(void* user, void* dst, size_t n);

struct MemoryReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    static size_t read(void* user, void* dst, size_t n);
};

// Geometry derived from the stream header.
struct Layout {
    int width = 0, height = 0;        // visible luma size
    int cwidth = 0, cheight = 0;      // visible chroma size, ceil(w/2) x ceil(h/2)
    int tile_w = 0, tile_h = 0;
    int cols = 0, rows = 0, tiles = 0;
    int pwidth = 0, pheight = 0;      // padded luma size (whole tiles)
    int pcwidth = 0, pcheight = 0;    // padded chroma size, ceil(pwidth/2) x ceil(pheight/2)
};

// Size in bytes of one tightly packed YUV420P frame.
size_t frame_size(int width, int height);

// Upper bound of one tile payload for the given tile size.
size_t worst_case_tile_bytes(int tile_w, int tile_h);

// Returns nullptr if the geometry is usable, otherwise a reason.
const char* validate_geometry(int width, int height, int tile_w, int tile_h);

Layout make_layout(int width, int height, int tile_w, int tile_h);

// Writes a TJC2 header (kStreamHeaderSize bytes).
void write_stream_header(const StreamHeader& h, uint8_t out[kStreamHeaderSize]);
// Parses a TJC1 header (size >= kStreamHeaderSizeV1) or TJC2 header
// (size >= kStreamHeaderSize). Returns Truncated if more bytes are needed.
Status parse_stream_header(const uint8_t* in, size_t size, StreamHeader* h);

// Low-level building blocks, exposed for tests and for anyone porting the hot loops.
namespace detail {

extern const uint8_t kZigzag[64];  // zigzag position -> natural (row-major) index

// Forward DCT of an 8x8 block of 8-bit samples (level-shifted internally).
// Output is natural order, scaled up by 8 relative to the orthonormal DCT.
void fdct8x8(const uint8_t* src, int stride, int32_t out[64]);

// Inverse DCT of dequantized natural-order coefficients (each within [-2048, 2047]).
void idct8x8(const int32_t in[64], uint8_t* dst, int stride);

// Fast path for blocks whose AC coefficients are all zero. Bit-identical to idct8x8.
void idct8x8_dc(int32_t dc, uint8_t* dst, int stride);

// Quant tables for a quality (1..100), natural order.
void build_quant_tables(int quality, uint16_t luma[64], uint16_t chroma[64]);

// Rounded division v / d (round half away from zero) done as a multiply by a
// precomputed reciprocal. Exact for |v| + d/2 < 2^20 and 1 <= d <= 4095, far beyond
// the range a forward DCT of 8-bit samples produces.
inline uint64_t quant_recip(uint32_t d) { return (uint64_t(1) << 40) / d + 1; }
inline int32_t quant_div(int32_t v, uint32_t d, uint64_t recip) {
    int32_t sign = v >> 31;  // 0 or -1; branch-free because most results are 0
    uint32_t a = uint32_t((v ^ sign) - sign) + (d >> 1);
    int32_t q = int32_t((a * recip) >> 40);
    return (q ^ sign) - sign;
}

struct HuffEnc {
    uint16_t code[256];
    uint8_t size[256];  // 0 = symbol not in table
};

struct HuffDec {
    uint16_t lut[512];  // 9-bit lookahead: (length << 8) | symbol, 0 = longer code
    int32_t maxcode[17];
    int32_t valoff[17];
    uint8_t vals[256];
};

struct HuffTables {
    HuffEnc enc_dc[2], enc_ac[2];  // [0] = luma, [1] = chroma
    HuffDec dec_dc[2], dec_ac[2];
};

const HuffTables& huff_tables();

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}
    void put(uint32_t bits, int len);
    void flush();  // pad the last byte with zeros

private:
    std::vector<uint8_t>& out_;
    uint32_t acc_ = 0;
    int n_ = 0;
};

class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : p_(data), size_(size) {}
    void refill() {
        while (cnt_ <= 24) {
            uint32_t b = pos_ < size_ ? p_[pos_] : 0u;
            ++pos_;
            buf_ |= b << (24 - cnt_);
            cnt_ += 8;
        }
    }
    uint32_t peek(int n) const { return buf_ >> (32 - n); }  // 1 <= n <= 24, after refill()
    void skip(int n) { buf_ <<= n; cnt_ -= n; }
    uint32_t get(int n) {
        if (n == 0) return 0;
        refill();
        uint32_t v = peek(n);
        skip(n);
        return v;
    }
    // True once more bits were consumed than the buffer holds.
    bool overrun() const { return pos_ * 8 - size_t(cnt_) > size_ * 8; }

private:
    const uint8_t* p_;
    size_t size_;
    size_t pos_ = 0;
    uint32_t buf_ = 0;
    int cnt_ = 0;
};

// Returns the decoded symbol, or -1 for an invalid code.
int huff_decode(BitReader& br, const HuffDec& t);

// Entropy-code one block of quantized coefficients given in zigzag order.
// Coefficients must lie in [-1023, 1023]. dc_pred is updated.
void encode_block(BitWriter& bw, const int16_t zz[64], int& dc_pred, int table);

// Decode one block into natural-order dequantized coefficients (clamped to
// [-2048, 2047]). dq is the quant table in zigzag order. dc_pred is updated.
// Returns -1 on bad data, 0 when only DC is present, 1 when any AC is present.
int decode_block(BitReader& br, int table, const uint16_t dq[64], int& dc_pred, int32_t coef[64]);

void pack_bitmap(const uint8_t* flags, int n, uint8_t* out);
void unpack_bitmap(const uint8_t* in, int n, uint8_t* flags);

// Sum of absolute differences between two w x h areas, stopping early once it
// exceeds `limit` (the returned value is then > limit but not exact).
uint64_t sad(const uint8_t* a, int astride, const uint8_t* b, int bstride, int w, int h, uint64_t limit);

class RefreshPolicy {
public:
    void reset(RefreshMode mode, uint32_t param, int tiles);
    // Whether frame `frame_index` (0-based count of frames since start) is a full refresh.
    bool forced(uint32_t frame_index) const;
    // Rolling mode: mark the next `param` tiles (round-robin) as dirty.
    void apply(uint8_t* dirty);
    int cursor() const { return cursor_; }

private:
    RefreshMode mode_ = RefreshMode::None;
    uint32_t param_ = 0;
    int tiles_ = 0;
    int cursor_ = 0;
};

// QOA audio frames (https://qoaformat.org). Bit-compatible with the reference
// qoa.h; frames are self-contained (each carries its predictor state).
namespace qoa {

constexpr int kSliceLen = 20;
constexpr int kMaxFrameLen = 256 * kSliceLen;  // samples per channel per frame
constexpr int kMaxChannels = 8;

struct Lms {
    int32_t history[4];
    int32_t weights[4];
};

// Initial predictor state used by the reference encoder.
void init_lms(Lms& lms);

size_t frame_size(int channels, int samples);

// Encodes 1..kMaxFrameLen interleaved samples per channel as one frame appended
// to out. lms (one per channel) carries the state from frame to frame; after the
// call it equals what a decoder reads from the next frame's header.
void encode_frame(const int16_t* interleaved, int channels, uint32_t rate, int samples, Lms* lms,
                  std::vector<uint8_t>& out);

// Decodes one frame at data. On success returns the samples per channel written
// to out (interleaved, room for kMaxFrameLen * channels needed) and sets *consumed;
// returns -1 for malformed data or a channel count / rate other than expected.
int decode_frame(const uint8_t* data, size_t size, int channels, uint32_t rate, int16_t* out,
                 size_t* consumed);

}  // namespace qoa

}  // namespace detail

// Padded planar YUV420P storage used by both encoder and decoder.
struct Planes {
    std::vector<uint8_t> p[3];
    int stride[3] = {0, 0, 0};
    bool alloc(const Layout& l);
};

class Decoder {
public:
    // Reads and validates the stream header (TJC1 or TJC2), allocates the
    // framebuffer (initialized to black). Must be called first.
    Status read_header(ReadFn fn, void* user);

    // Reads and applies one frame record. Returns EndOfStream at clean EOF.
    Status decode_frame(ReadFn fn, void* user, FrameStats* stats = nullptr);

    const StreamHeader& header() const { return header_; }
    const Layout& layout() const { return layout_; }

    // Copies the current visible frame as tightly packed YUV420P (frame_size() bytes).
    void copy_frame(uint8_t* dst) const;

    // Direct access to the padded planes (0 = Y, 1 = Cb, 2 = Cr).
    const uint8_t* plane(int c) const { return fb_.p[c].data(); }
    int stride(int c) const { return fb_.stride[c]; }

    // Audio that belongs to the last decoded frame: audio_samples() samples per
    // channel, interleaved, header().audio_channels channels. Empty without audio.
    const int16_t* audio() const { return audio_.data(); }
    size_t audio_samples() const { return audio_samples_; }

private:
    bool decode_tile(const uint8_t* data, size_t size, int tile);
    Status decode_audio(ReadFn fn, void* user, size_t* bytes);

    StreamHeader header_;
    Layout layout_;
    Planes fb_;
    uint16_t dq_[2][64] = {};  // dequant tables, zigzag order
    std::vector<uint8_t> dirty_, bitmap_, payload_;
    std::vector<int16_t> audio_;
    size_t audio_samples_ = 0;
    bool ready_ = false;
};

#ifndef TJC_NO_ENCODER
class Encoder {
public:
    // Validates the config and allocates buffers. On failure returns false and
    // error() says why.
    bool init(const Config& cfg);
    const char* error() const { return error_; }

    const Config& config() const { return cfg_; }
    const Layout& layout() const { return layout_; }
    StreamHeader stream_header() const;

    // Appends the 18-byte stream header.
    void write_stream_header(std::vector<uint8_t>& out) const;

    // Encodes one tightly packed YUV420P frame of frame_size(width, height) bytes
    // and appends the frame record to out.
    void encode_frame(const uint8_t* yuv420p, std::vector<uint8_t>& out, FrameStats* stats = nullptr);

    // Same, with separate planes and strides.
    void encode_frame(const uint8_t* y, int ystride, const uint8_t* u, int ustride,
                      const uint8_t* v, int vstride, std::vector<uint8_t>& out,
                      FrameStats* stats = nullptr);

    // Make the next frame a full refresh (e.g. a new receiver joined).
    void force_keyframe() { keyframe_request_ = true; }

    // Queues interleaved audio (config().audio_channels channels). Each
    // encode_frame() takes samples_for_next_frame() samples per channel from the
    // queue, padding with silence if it runs short. Push ahead of the video.
    void push_audio(const int16_t* interleaved, size_t samples_per_channel);
    size_t samples_for_next_frame() const;
    size_t queued_audio() const { return audio_queue_.size() / (cfg_.audio_channels ? cfg_.audio_channels : 1); }

    // The encoder's shadow copy of the decoder framebuffer, as tightly packed YUV420P.
    void copy_recon(uint8_t* dst) const;

private:
    void load(const uint8_t* y, int ys, const uint8_t* u, int us, const uint8_t* v, int vs);
    bool tile_changed(int tile) const;
    void encode_tile(int tile, std::vector<uint8_t>& out);
    void copy_tile(const Planes& from, Planes& to, int tile);
    template <class F> void parallel_for(int n, int min_per_thread, F fn);

    Config cfg_;
    Layout layout_;
    Planes cur_, last_coded_, recon_;
    uint16_t q_[2][64] = {};      // natural order
    uint32_t qdiv_[2][64] = {};   // zigzag order: 8 * q, the forward DCT's scale included
    uint64_t qrecip_[2][64] = {}; // zigzag order: detail::quant_recip(qdiv_)
    detail::RefreshPolicy policy_;
    std::vector<uint8_t> dirty_;
    std::vector<int> dirty_list_;
    std::vector<std::vector<uint8_t>> worker_out_;
    int threads_ = 1;
    uint32_t frame_num_ = 0;
    uint32_t frame_index_ = 0;
    bool keyframe_request_ = false;
    std::vector<int16_t> audio_queue_;
    detail::qoa::Lms audio_lms_[detail::qoa::kMaxChannels] = {};
    uint64_t audio_rem_ = 0;  // remainder of the samples-per-frame division
    bool ready_ = false;
    const char* error_ = "not initialized";
};
#endif  // TJC_NO_ENCODER

}  // namespace tjc

#endif  // TJC_H_INCLUDED

// ===========================================================================
// Implementation
// ===========================================================================
#ifdef TJC_IMPLEMENTATION
#ifndef TJC_IMPLEMENTATION_DONE
#define TJC_IMPLEMENTATION_DONE

#include <algorithm>
#include <cstring>
#if !defined(TJC_NO_ENCODER) && !defined(TJC_NO_THREADS)
#include <thread>
#endif

static_assert((-5 >> 1) == -3, "tjc needs arithmetic right shift of negative integers");

namespace tjc {

// ---------------------------------------------------------------------------
// Byte order helpers
// ---------------------------------------------------------------------------
namespace {

inline void put_u16(uint8_t* p, uint32_t v) {
#if TJC_STREAM_BIG_ENDIAN
    p[0] = uint8_t(v >> 8); p[1] = uint8_t(v);
#else
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8);
#endif
}

inline void put_u32(uint8_t* p, uint32_t v) {
#if TJC_STREAM_BIG_ENDIAN
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
#else
    p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24);
#endif
}

inline uint32_t get_u16(const uint8_t* p) {
#if TJC_STREAM_BIG_ENDIAN
    return (uint32_t(p[0]) << 8) | p[1];
#else
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8);
#endif
}

inline uint32_t get_u32(const uint8_t* p) {
#if TJC_STREAM_BIG_ENDIAN
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
#else
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
#endif
}

inline uint8_t clamp_u8(int32_t v) { return uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// Reads exactly n bytes unless the input ends; returns the count read.
size_t read_full(ReadFn fn, void* user, void* dst, size_t n) {
    size_t got = 0;
    uint8_t* d = static_cast<uint8_t*>(dst);
    while (got < n) {
        size_t r = fn(user, d + got, n - got);
        if (r == 0) break;
        got += r;
    }
    return got;
}

const uint8_t kStdLumaQ[64] = {
    16, 11, 10, 16, 24, 40, 51, 61,
    12, 12, 14, 19, 26, 58, 60, 55,
    14, 13, 16, 24, 40, 57, 69, 56,
    14, 17, 22, 29, 51, 87, 80, 62,
    18, 22, 37, 56, 68, 109, 103, 77,
    24, 35, 55, 64, 81, 104, 113, 92,
    49, 64, 78, 87, 103, 121, 120, 101,
    72, 92, 95, 98, 112, 100, 103, 99,
};

const uint8_t kStdChromaQ[64] = {
    17, 18, 24, 47, 99, 99, 99, 99,
    18, 21, 26, 66, 99, 99, 99, 99,
    24, 26, 56, 99, 99, 99, 99, 99,
    47, 66, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
    99, 99, 99, 99, 99, 99, 99, 99,
};

// Standard JPEG Huffman tables (ITU T.81 Annex K.3).
const uint8_t kDcLumaBits[16] = {0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
const uint8_t kDcLumaVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
const uint8_t kDcChromaBits[16] = {0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
const uint8_t kDcChromaVals[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

const uint8_t kAcLumaBits[16] = {0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
const uint8_t kAcLumaVals[162] = {
    0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
    0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
    0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
    0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
    0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
    0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
    0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
    0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
    0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

const uint8_t kAcChromaBits[16] = {0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
const uint8_t kAcChromaVals[162] = {
    0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
    0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
    0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
    0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
    0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
    0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
    0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
    0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa,
};

void build_huff(const uint8_t bits[16], const uint8_t* vals, detail::HuffEnc& enc, detail::HuffDec& dec) {
    std::memset(&enc, 0, sizeof(enc));
    std::memset(&dec, 0, sizeof(dec));
    uint32_t code = 0;
    int k = 0;
    for (int len = 1; len <= 16; ++len) {
        int n = bits[len - 1];
        dec.maxcode[len] = -1;
        if (n) {
            dec.valoff[len] = k - int32_t(code);
            for (int i = 0; i < n; ++i, ++k, ++code) {
                uint8_t sym = vals[k];
                dec.vals[k] = sym;
                enc.code[sym] = uint16_t(code);
                enc.size[sym] = uint8_t(len);
                if (len <= 9) {
                    uint32_t first = code << (9 - len);
                    uint32_t count = 1u << (9 - len);
                    for (uint32_t j = 0; j < count; ++j) dec.lut[first + j] = uint16_t((len << 8) | sym);
                }
            }
            dec.maxcode[len] = int32_t(code) - 1;
        }
        code <<= 1;
    }
}

inline int bit_count(int v) {
    int n = 0;
    while (v) { ++n; v >>= 1; }
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// Public helpers
// ---------------------------------------------------------------------------
const char* status_string(Status s) {
    switch (s) {
        case Status::Ok: return "ok";
        case Status::EndOfStream: return "end of stream";
        case Status::Truncated: return "truncated input";
        case Status::BadHeader: return "bad or unsupported stream header";
        case Status::Corrupt: return "corrupt frame data";
        case Status::OutOfMemory: return "out of memory";
        case Status::NotInitialized: return "stream header not read yet";
    }
    return "unknown";
}

size_t MemoryReader::read(void* user, void* dst, size_t n) {
    MemoryReader* r = static_cast<MemoryReader*>(user);
    size_t left = r->size - r->pos;
    if (n > left) n = left;
    std::memcpy(dst, r->data + r->pos, n);
    r->pos += n;
    return n;
}

size_t frame_size(int width, int height) {
    size_t cw = size_t(width + 1) / 2, ch = size_t(height + 1) / 2;
    return size_t(width) * size_t(height) + 2 * cw * ch;
}

size_t worst_case_tile_bytes(int tile_w, int tile_h) {
    // Luma blocks plus the most chroma blocks any tile position can own.
    // Per block: DC <= 16+11 bits, 63 AC <= 16+10 bits.
    size_t luma = size_t((tile_w + 7) / 8) * size_t((tile_h + 7) / 8);
    size_t chroma = size_t(((tile_w + 1) / 2 + 7) / 8) * size_t(((tile_h + 1) / 2 + 7) / 8);
    return ((luma + 2 * chroma) * (27 + 63 * 26) + 7) / 8;
}

const char* validate_geometry(int width, int height, int tile_w, int tile_h) {
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535) return "width/height must be 1..65535";
    if (tile_w < 1 || tile_h < 1 || tile_w > 255 || tile_h > 255) return "tile width/height must be 1..255";
    if (worst_case_tile_bytes(tile_w, tile_h) > size_t(kMaxTilePayload))
        return "tile too large: worst-case payload would not fit the 16-bit tile_len (keep w*h <= 12544, e.g. 112x112 or 128x64)";
    return nullptr;
}

Layout make_layout(int width, int height, int tile_w, int tile_h) {
    Layout l;
    l.width = width;
    l.height = height;
    l.cwidth = (width + 1) / 2;
    l.cheight = (height + 1) / 2;
    l.tile_w = tile_w;
    l.tile_h = tile_h;
    l.cols = (width + tile_w - 1) / tile_w;
    l.rows = (height + tile_h - 1) / tile_h;
    l.tiles = l.cols * l.rows;
    l.pwidth = l.cols * tile_w;
    l.pheight = l.rows * tile_h;
    l.pcwidth = (l.pwidth + 1) / 2;
    l.pcheight = (l.pheight + 1) / 2;
    return l;
}

void write_stream_header(const StreamHeader& h, uint8_t out[kStreamHeaderSize]) {
    std::memset(out, 0, kStreamHeaderSize);
    out[0] = 'T'; out[1] = 'J'; out[2] = 'C'; out[3] = '2';
    put_u16(out + 4, h.width);
    put_u16(out + 6, h.height);
    out[8] = h.tile_w;
    out[9] = h.tile_h;
    out[10] = h.chroma_format;
    out[11] = uint8_t(h.refresh_mode);
    put_u16(out + 12, h.refresh_param);
    out[14] = h.quality;
    put_u32(out + 18, h.fps_num);
    put_u32(out + 22, h.fps_den);
    out[26] = h.audio_codec;
    out[27] = h.audio_codec ? h.audio_channels : 0;
    put_u32(out + 28, h.audio_codec ? h.audio_rate : 0);
}

Status parse_stream_header(const uint8_t* in, size_t size, StreamHeader* h) {
    if (size < 4) return Status::Truncated;
    if (in[0] != 'T' || in[1] != 'J' || in[2] != 'C' || (in[3] != '1' && in[3] != '2')) return Status::BadHeader;
    StreamHeader r;
    r.version = uint8_t(in[3] - '0');
    if (size < size_t(r.version == 1 ? kStreamHeaderSizeV1 : kStreamHeaderSize)) return Status::Truncated;
    if (r.version == 2) {
        r.fps_num = get_u32(in + 18);
        r.fps_den = get_u32(in + 22);
        r.audio_codec = in[26];
        if (r.fps_num == 0 || r.fps_den == 0 || r.audio_codec > 1) return Status::BadHeader;
        if (r.audio_codec) {
            r.audio_channels = in[27];
            r.audio_rate = get_u32(in + 28);
            if (r.audio_channels < 1 || r.audio_channels > detail::qoa::kMaxChannels || r.audio_rate < 1 ||
                r.audio_rate > 0xffffff)
                return Status::BadHeader;
        }
    }
    r.width = uint16_t(get_u16(in + 4));
    r.height = uint16_t(get_u16(in + 6));
    r.tile_w = in[8];
    r.tile_h = in[9];
    r.chroma_format = in[10];
    if (in[11] > 2) return Status::BadHeader;
    r.refresh_mode = RefreshMode(in[11]);
    r.refresh_param = uint16_t(get_u16(in + 12));
    r.quality = in[14];
    if (r.chroma_format != 0 || r.quality < 1 || r.quality > 100) return Status::BadHeader;
    if (validate_geometry(r.width, r.height, r.tile_w, r.tile_h)) return Status::BadHeader;
    *h = r;
    return Status::Ok;
}

bool Planes::alloc(const Layout& l) {
    size_t ls = size_t(l.pwidth) * size_t(l.pheight);
    size_t cs = size_t(l.pcwidth) * size_t(l.pcheight);
    try {
        p[0].assign(ls, 16);
        p[1].assign(cs, 128);
        p[2].assign(cs, 128);
    } catch (...) {
        return false;
    }
    stride[0] = l.pwidth;
    stride[1] = stride[2] = l.pcwidth;
    return true;
}

namespace {

// Visible plane geometry for component c.
inline void visible_size(const Layout& l, int c, int& w, int& h) {
    w = c ? l.cwidth : l.width;
    h = c ? l.cheight : l.height;
}

// Tile rectangle of component c within its padded plane. A chroma sample belongs
// to the tile holding its top-left luma pixel; w or h is 0 when a tile owns none.
inline void tile_rect(const Layout& l, int tile, int c, int& x, int& y, int& w, int& h) {
    int x0 = (tile % l.cols) * l.tile_w, y0 = (tile / l.cols) * l.tile_h;
    if (c == 0) {
        x = x0;
        y = y0;
        w = l.tile_w;
        h = l.tile_h;
        return;
    }
    x = (x0 + 1) / 2;
    y = (y0 + 1) / 2;
    w = (x0 + l.tile_w + 1) / 2 - x;
    h = (y0 + l.tile_h + 1) / 2 - y;
}

// Copies a bw x bh area (bw, bh <= 8) into an 8x8 block, replicating the last
// column and row to fill it.
inline void load_partial_block(const uint8_t* src, int stride, int bw, int bh, uint8_t blk[64]) {
    for (int y = 0; y < 8; ++y) {
        const uint8_t* s = src + size_t(y < bh ? y : bh - 1) * stride;
        uint8_t* d = blk + y * 8;
        std::memcpy(d, s, size_t(bw));
        std::memset(d + bw, s[bw - 1], size_t(8 - bw));
    }
}

inline void store_partial_block(const uint8_t blk[64], uint8_t* dst, int stride, int bw, int bh) {
    for (int y = 0; y < bh; ++y) std::memcpy(dst + size_t(y) * stride, blk + y * 8, size_t(bw));
}

void copy_cropped(const Planes& src, const Layout& l, uint8_t* dst) {
    for (int c = 0; c < 3; ++c) {
        int w, h;
        visible_size(l, c, w, h);
        const uint8_t* s = src.p[c].data();
        for (int y = 0; y < h; ++y, dst += w) std::memcpy(dst, s + size_t(y) * src.stride[c], size_t(w));
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// detail: DCT, quantization, Huffman, bitstream, bitmap, SAD, refresh policy
// ---------------------------------------------------------------------------
namespace detail {

const uint8_t kZigzag[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

// Loeffler-Ligtenberg-Moschytz integer DCT, as in the IJG "islow" code:
// 13-bit fixed-point constants, 2 extra bits of precision between passes.
namespace {
constexpr int kConstBits = 13;
constexpr int kPass1Bits = 2;
constexpr int32_t FIX_0_298631336 = 2446;
constexpr int32_t FIX_0_390180644 = 3196;
constexpr int32_t FIX_0_541196100 = 4433;
constexpr int32_t FIX_0_765366865 = 6270;
constexpr int32_t FIX_0_899976223 = 7373;
constexpr int32_t FIX_1_175875602 = 9633;
constexpr int32_t FIX_1_501321110 = 12299;
constexpr int32_t FIX_1_847759065 = 15137;
constexpr int32_t FIX_1_961570560 = 16069;
constexpr int32_t FIX_2_053119869 = 16819;
constexpr int32_t FIX_2_562915447 = 20995;
constexpr int32_t FIX_3_072711026 = 25172;

inline int32_t descale(int32_t x, int n) { return (x + (int32_t(1) << (n - 1))) >> n; }
}  // namespace

void fdct8x8(const uint8_t* src, int stride, int32_t out[64]) {
    int32_t ws[64];
    for (int y = 0; y < 8; ++y) {
        const uint8_t* s = src + size_t(y) * stride;
        int32_t* o = ws + y * 8;
        int32_t tmp0 = s[0] + s[7] - 256, tmp7 = s[0] - s[7];
        int32_t tmp1 = s[1] + s[6] - 256, tmp6 = s[1] - s[6];
        int32_t tmp2 = s[2] + s[5] - 256, tmp5 = s[2] - s[5];
        int32_t tmp3 = s[3] + s[4] - 256, tmp4 = s[3] - s[4];

        int32_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3;
        int32_t tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        o[0] = (tmp10 + tmp11) * (1 << kPass1Bits);
        o[4] = (tmp10 - tmp11) * (1 << kPass1Bits);
        int32_t z1 = (tmp12 + tmp13) * FIX_0_541196100;
        o[2] = descale(z1 + tmp13 * FIX_0_765366865, kConstBits - kPass1Bits);
        o[6] = descale(z1 - tmp12 * FIX_1_847759065, kConstBits - kPass1Bits);

        z1 = tmp4 + tmp7;
        int32_t z2 = tmp5 + tmp6, z3 = tmp4 + tmp6, z4 = tmp5 + tmp7;
        int32_t z5 = (z3 + z4) * FIX_1_175875602;
        tmp4 *= FIX_0_298631336; tmp5 *= FIX_2_053119869;
        tmp6 *= FIX_3_072711026; tmp7 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223; z2 *= -FIX_2_562915447;
        z3 *= -FIX_1_961570560; z4 *= -FIX_0_390180644;
        z3 += z5; z4 += z5;
        o[7] = descale(tmp4 + z1 + z3, kConstBits - kPass1Bits);
        o[5] = descale(tmp5 + z2 + z4, kConstBits - kPass1Bits);
        o[3] = descale(tmp6 + z2 + z3, kConstBits - kPass1Bits);
        o[1] = descale(tmp7 + z1 + z4, kConstBits - kPass1Bits);
    }
    for (int x = 0; x < 8; ++x) {
        const int32_t* c = ws + x;
        int32_t* o = out + x;
        int32_t tmp0 = c[0] + c[56], tmp7 = c[0] - c[56];
        int32_t tmp1 = c[8] + c[48], tmp6 = c[8] - c[48];
        int32_t tmp2 = c[16] + c[40], tmp5 = c[16] - c[40];
        int32_t tmp3 = c[24] + c[32], tmp4 = c[24] - c[32];

        int32_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3;
        int32_t tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;
        o[0] = descale(tmp10 + tmp11, kPass1Bits);
        o[32] = descale(tmp10 - tmp11, kPass1Bits);
        int32_t z1 = (tmp12 + tmp13) * FIX_0_541196100;
        o[16] = descale(z1 + tmp13 * FIX_0_765366865, kConstBits + kPass1Bits);
        o[48] = descale(z1 - tmp12 * FIX_1_847759065, kConstBits + kPass1Bits);

        z1 = tmp4 + tmp7;
        int32_t z2 = tmp5 + tmp6, z3 = tmp4 + tmp6, z4 = tmp5 + tmp7;
        int32_t z5 = (z3 + z4) * FIX_1_175875602;
        tmp4 *= FIX_0_298631336; tmp5 *= FIX_2_053119869;
        tmp6 *= FIX_3_072711026; tmp7 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223; z2 *= -FIX_2_562915447;
        z3 *= -FIX_1_961570560; z4 *= -FIX_0_390180644;
        z3 += z5; z4 += z5;
        o[56] = descale(tmp4 + z1 + z3, kConstBits + kPass1Bits);
        o[40] = descale(tmp5 + z2 + z4, kConstBits + kPass1Bits);
        o[24] = descale(tmp6 + z2 + z3, kConstBits + kPass1Bits);
        o[8] = descale(tmp7 + z1 + z4, kConstBits + kPass1Bits);
    }
}

void idct8x8(const int32_t in[64], uint8_t* dst, int stride) {
    int32_t ws[64];
    // Pass 1: columns. Inputs within 11 bits + sign keep every product in 32 bits.
    for (int x = 0; x < 8; ++x) {
        const int32_t* c = in + x;
        int32_t* w = ws + x;
        if ((c[8] | c[16] | c[24] | c[32] | c[40] | c[48] | c[56]) == 0) {
            int32_t dc = c[0] * (1 << kPass1Bits);
            w[0] = w[8] = w[16] = w[24] = w[32] = w[40] = w[48] = w[56] = dc;
            continue;
        }
        int32_t z2 = c[16], z3 = c[48];
        int32_t z1 = (z2 + z3) * FIX_0_541196100;
        int32_t tmp2 = z1 - z3 * FIX_1_847759065;
        int32_t tmp3 = z1 + z2 * FIX_0_765366865;
        z2 = c[0];
        z3 = c[32];
        int32_t tmp0 = (z2 + z3) * (1 << kConstBits);
        int32_t tmp1 = (z2 - z3) * (1 << kConstBits);
        int32_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3;
        int32_t tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;

        tmp0 = c[56]; tmp1 = c[40]; tmp2 = c[24]; tmp3 = c[8];
        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        int32_t z4 = tmp1 + tmp3;
        int32_t z5 = (z3 + z4) * FIX_1_175875602;
        tmp0 *= FIX_0_298631336; tmp1 *= FIX_2_053119869;
        tmp2 *= FIX_3_072711026; tmp3 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223; z2 *= -FIX_2_562915447;
        z3 *= -FIX_1_961570560; z4 *= -FIX_0_390180644;
        z3 += z5; z4 += z5;
        tmp0 += z1 + z3; tmp1 += z2 + z4; tmp2 += z2 + z3; tmp3 += z1 + z4;

        const int sh = kConstBits - kPass1Bits;
        w[0] = descale(tmp10 + tmp3, sh);
        w[56] = descale(tmp10 - tmp3, sh);
        w[8] = descale(tmp11 + tmp2, sh);
        w[48] = descale(tmp11 - tmp2, sh);
        w[16] = descale(tmp12 + tmp1, sh);
        w[40] = descale(tmp12 - tmp1, sh);
        w[24] = descale(tmp13 + tmp0, sh);
        w[32] = descale(tmp13 - tmp0, sh);
    }
    // Pass 2: rows, removing the remaining scale (incl. the factor of 8) and level shift.
    const int sh = kConstBits + kPass1Bits + 3;
    for (int y = 0; y < 8; ++y) {
        const int32_t* r = ws + y * 8;
        uint8_t* o = dst + size_t(y) * stride;
        if ((r[1] | r[2] | r[3] | r[4] | r[5] | r[6] | r[7]) == 0) {
            uint8_t v = clamp_u8(descale(r[0] * (1 << kConstBits), sh) + 128);
            std::memset(o, v, 8);
            continue;
        }
        int32_t z2 = r[2], z3 = r[6];
        int32_t z1 = (z2 + z3) * FIX_0_541196100;
        int32_t tmp2 = z1 - z3 * FIX_1_847759065;
        int32_t tmp3 = z1 + z2 * FIX_0_765366865;
        int32_t tmp0 = (r[0] + r[4]) * (1 << kConstBits);
        int32_t tmp1 = (r[0] - r[4]) * (1 << kConstBits);
        int32_t tmp10 = tmp0 + tmp3, tmp13 = tmp0 - tmp3;
        int32_t tmp11 = tmp1 + tmp2, tmp12 = tmp1 - tmp2;

        tmp0 = r[7]; tmp1 = r[5]; tmp2 = r[3]; tmp3 = r[1];
        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        int32_t z4 = tmp1 + tmp3;
        int32_t z5 = (z3 + z4) * FIX_1_175875602;
        tmp0 *= FIX_0_298631336; tmp1 *= FIX_2_053119869;
        tmp2 *= FIX_3_072711026; tmp3 *= FIX_1_501321110;
        z1 *= -FIX_0_899976223; z2 *= -FIX_2_562915447;
        z3 *= -FIX_1_961570560; z4 *= -FIX_0_390180644;
        z3 += z5; z4 += z5;
        tmp0 += z1 + z3; tmp1 += z2 + z4; tmp2 += z2 + z3; tmp3 += z1 + z4;

        o[0] = clamp_u8(descale(tmp10 + tmp3, sh) + 128);
        o[7] = clamp_u8(descale(tmp10 - tmp3, sh) + 128);
        o[1] = clamp_u8(descale(tmp11 + tmp2, sh) + 128);
        o[6] = clamp_u8(descale(tmp11 - tmp2, sh) + 128);
        o[2] = clamp_u8(descale(tmp12 + tmp1, sh) + 128);
        o[5] = clamp_u8(descale(tmp12 - tmp1, sh) + 128);
        o[3] = clamp_u8(descale(tmp13 + tmp0, sh) + 128);
        o[4] = clamp_u8(descale(tmp13 - tmp0, sh) + 128);
    }
}

void idct8x8_dc(int32_t dc, uint8_t* dst, int stride) {
    // Same arithmetic idct8x8 performs when only coefficient 0 is non-zero.
    int32_t ws = dc * (1 << kPass1Bits);
    uint8_t v = clamp_u8(descale(ws * (1 << kConstBits), kConstBits + kPass1Bits + 3) + 128);
    for (int y = 0; y < 8; ++y) std::memset(dst + size_t(y) * stride, v, 8);
}

void build_quant_tables(int quality, uint16_t luma[64], uint16_t chroma[64]) {
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    int scale = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for (int i = 0; i < 64; ++i) {
        int l = (kStdLumaQ[i] * scale + 50) / 100;
        int c = (kStdChromaQ[i] * scale + 50) / 100;
        luma[i] = uint16_t(std::min(255, std::max(1, l)));
        chroma[i] = uint16_t(std::min(255, std::max(1, c)));
    }
}

const HuffTables& huff_tables() {
    static const HuffTables tables = [] {
        HuffTables t;
        build_huff(kDcLumaBits, kDcLumaVals, t.enc_dc[0], t.dec_dc[0]);
        build_huff(kDcChromaBits, kDcChromaVals, t.enc_dc[1], t.dec_dc[1]);
        build_huff(kAcLumaBits, kAcLumaVals, t.enc_ac[0], t.dec_ac[0]);
        build_huff(kAcChromaBits, kAcChromaVals, t.enc_ac[1], t.dec_ac[1]);
        return t;
    }();
    return tables;
}

void BitWriter::put(uint32_t bits, int len) {
    if (len == 0) return;
    acc_ = (acc_ << len) | (bits & ((1u << len) - 1));
    n_ += len;
    while (n_ >= 8) {
        n_ -= 8;
        out_.push_back(uint8_t(acc_ >> n_));
    }
}

void BitWriter::flush() {
    if (n_ > 0) out_.push_back(uint8_t(acc_ << (8 - n_)));
    acc_ = 0;
    n_ = 0;
}

int huff_decode(BitReader& br, const HuffDec& t) {
    br.refill();
    uint32_t e = t.lut[br.peek(9)];
    if (e) {
        br.skip(int(e >> 8));
        return int(e & 0xff);
    }
    for (int len = 10; len <= 16; ++len) {
        int32_t code = int32_t(br.peek(len));
        if (code <= t.maxcode[len]) {
            br.skip(len);
            return t.vals[code + t.valoff[len]];
        }
    }
    return -1;
}

void encode_block(BitWriter& bw, const int16_t zz[64], int& dc_pred, int table) {
    const HuffTables& ht = huff_tables();
    const HuffEnc& dc = ht.enc_dc[table];
    const HuffEnc& ac = ht.enc_ac[table];

    int diff = zz[0] - dc_pred;
    dc_pred = zz[0];
    int s = bit_count(diff < 0 ? -diff : diff);
    bw.put(dc.code[s], dc.size[s]);
    bw.put(uint32_t(diff < 0 ? diff - 1 : diff), s);

    int run = 0;
    for (int k = 1; k < 64; ++k) {
        int v = zz[k];
        if (v == 0) {
            ++run;
            continue;
        }
        while (run > 15) {
            bw.put(ac.code[0xf0], ac.size[0xf0]);
            run -= 16;
        }
        s = bit_count(v < 0 ? -v : v);
        int sym = (run << 4) | s;
        bw.put(ac.code[sym], ac.size[sym]);
        bw.put(uint32_t(v < 0 ? v - 1 : v), s);
        run = 0;
    }
    if (run) bw.put(ac.code[0x00], ac.size[0x00]);
}

int decode_block(BitReader& br, int table, const uint16_t dq[64], int& dc_pred, int32_t coef[64]) {
    const HuffTables& ht = huff_tables();
    std::memset(coef, 0, 64 * sizeof(int32_t));

    int s = huff_decode(br, ht.dec_dc[table]);
    if (s < 0 || s > 11) return -1;
    int diff = 0;
    if (s) {
        diff = int(br.get(s));
        if (diff < (1 << (s - 1))) diff -= (1 << s) - 1;
    }
    dc_pred += diff;
    if (dc_pred < -2047 || dc_pred > 2047) return -1;
    int32_t v = dc_pred * int32_t(dq[0]);
    coef[0] = std::min<int32_t>(2047, std::max<int32_t>(-2048, v));

    const HuffDec& ac = ht.dec_ac[table];
    int has_ac = 0;
    for (int k = 1; k < 64;) {
        int rs = huff_decode(br, ac);
        if (rs < 0) return -1;
        int r = rs >> 4;
        s = rs & 15;
        if (s == 0) {
            if (r != 15) break;  // EOB
            k += 16;             // ZRL
            continue;
        }
        k += r;
        if (k > 63) return -1;
        int a = int(br.get(s));
        if (a < (1 << (s - 1))) a -= (1 << s) - 1;
        v = a * int32_t(dq[k]);
        coef[kZigzag[k]] = std::min<int32_t>(2047, std::max<int32_t>(-2048, v));
        has_ac = 1;
        ++k;
    }
    return has_ac;
}

void pack_bitmap(const uint8_t* flags, int n, uint8_t* out) {
    std::memset(out, 0, size_t(n + 7) / 8);
    for (int i = 0; i < n; ++i)
        if (flags[i]) out[i >> 3] |= uint8_t(1u << (i & 7));
}

void unpack_bitmap(const uint8_t* in, int n, uint8_t* flags) {
    for (int i = 0; i < n; ++i) flags[i] = (in[i >> 3] >> (i & 7)) & 1;
}

uint64_t sad(const uint8_t* a, int astride, const uint8_t* b, int bstride, int w, int h, uint64_t limit) {
    uint64_t total = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t* pa = a + size_t(y) * astride;
        const uint8_t* pb = b + size_t(y) * bstride;
        uint32_t row = 0;
        for (int x = 0; x < w; ++x) {
            int d = int(pa[x]) - int(pb[x]);
            row += uint32_t(d < 0 ? -d : d);
        }
        total += row;
        if (total > limit) return total;
    }
    return total;
}

void RefreshPolicy::reset(RefreshMode mode, uint32_t param, int tiles) {
    mode_ = mode;
    param_ = param;
    tiles_ = tiles;
    cursor_ = 0;
}

bool RefreshPolicy::forced(uint32_t frame_index) const {
    if (frame_index == 0) return true;
    return mode_ == RefreshMode::FullPeriodic && param_ && frame_index % param_ == 0;
}

void RefreshPolicy::apply(uint8_t* dirty) {
    if (mode_ != RefreshMode::Rolling || param_ == 0 || tiles_ == 0) return;
    uint32_t n = std::min<uint32_t>(param_, uint32_t(tiles_));
    for (uint32_t i = 0; i < n; ++i) {
        dirty[cursor_] = 1;
        if (++cursor_ == tiles_) cursor_ = 0;
    }
}

// ---------------------------------------------------------------------------
// QOA audio
//
// Port of the QOA reference coder (https://github.com/phoboslab/qoa):
//   Copyright (c) 2023, Dominic Szablewski - https://phoboslab.org
//   SPDX-License-Identifier: MIT
//   Permission is hereby granted, free of charge, to any person obtaining a copy
//   of this software and associated documentation files (the "Software"), to deal
//   in the Software without restriction, including without limitation the rights
//   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//   copies of the Software, and to permit persons to whom the Software is
//   furnished to do so, subject to the following conditions: The above copyright
//   notice and this permission notice shall be included in all copies or
//   substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS",
//   WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
//
// Differences from the reference, none of which change valid streams:
//   - The encoder continues each frame from the int16 predictor state it wrote
//     into the frame header, so it always matches the decoder exactly.
//   - Sums that could overflow 32 bits on hostile input use 64 bits.
// ---------------------------------------------------------------------------
namespace qoa {
namespace {

const int32_t kReciprocal[16] = {65536, 9363, 3121, 1457, 781, 475, 311, 216,
                                 156,   117,  90,   71,   57,  47,  39,  32};
const uint8_t kQuant[17] = {7, 7, 7, 5, 5, 3, 3, 1, 0, 0, 2, 2, 4, 4, 6, 6, 6};  // index: clamped residual + 8
const int16_t kDequant[16][8] = {
    {1, -1, 3, -3, 5, -5, 7, -7},
    {5, -5, 18, -18, 32, -32, 49, -49},
    {16, -16, 53, -53, 95, -95, 147, -147},
    {34, -34, 113, -113, 203, -203, 315, -315},
    {63, -63, 210, -210, 378, -378, 588, -588},
    {104, -104, 345, -345, 621, -621, 966, -966},
    {158, -158, 528, -528, 950, -950, 1477, -1477},
    {228, -228, 760, -760, 1368, -1368, 2128, -2128},
    {316, -316, 1053, -1053, 1895, -1895, 2947, -2947},
    {422, -422, 1405, -1405, 2529, -2529, 3934, -3934},
    {548, -548, 1828, -1828, 3290, -3290, 5117, -5117},
    {696, -696, 2320, -2320, 4176, -4176, 6496, -6496},
    {868, -868, 2893, -2893, 5207, -5207, 8099, -8099},
    {1064, -1064, 3548, -3548, 6386, -6386, 9933, -9933},
    {1286, -1286, 4288, -4288, 7718, -7718, 12005, -12005},
    {1536, -1536, 5120, -5120, 9216, -9216, 14336, -14336},
};

inline int32_t predict(const Lms& l) {
    int64_t p = 0;
    for (int i = 0; i < 4; ++i) p += int64_t(l.weights[i]) * l.history[i];
    p >>= 13;
    // Valid streams stay far inside this range; it only tames hostile input.
    return int32_t(std::min<int64_t>(1 << 24, std::max<int64_t>(-(1 << 24), p)));
}

inline void update(Lms& l, int32_t sample, int32_t residual) {
    int32_t delta = residual >> 4;
    for (int i = 0; i < 4; ++i) l.weights[i] += l.history[i] < 0 ? -delta : delta;
    l.history[0] = l.history[1];
    l.history[1] = l.history[2];
    l.history[2] = l.history[3];
    l.history[3] = sample;
}

inline int32_t clamp_s16(int32_t v) { return v < -32768 ? -32768 : (v > 32767 ? 32767 : v); }

// Rounding division by a scalefactor that never rounds a non-zero value to zero.
inline int32_t div_sf(int32_t v, int sf) {
    int32_t n = int32_t((int64_t(v) * kReciprocal[sf] + (1 << 15)) >> 16);
    return n + ((v > 0) - (v < 0)) - ((n > 0) - (n < 0));
}

inline void put_u64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 56; i >= 0; i -= 8) out.push_back(uint8_t(v >> i));
}

inline uint64_t get_u64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

}  // namespace

void init_lms(Lms& lms) {
    for (int i = 0; i < 4; ++i) lms.history[i] = 0;
    lms.weights[0] = 0;
    lms.weights[1] = 0;
    lms.weights[2] = -(1 << 13);
    lms.weights[3] = 1 << 14;
}

size_t frame_size(int channels, int samples) {
    size_t slices = size_t(samples + kSliceLen - 1) / kSliceLen;
    return 8 + 16 * size_t(channels) + 8 * slices * size_t(channels);
}

void encode_frame(const int16_t* interleaved, int channels, uint32_t rate, int samples, Lms* lms,
                  std::vector<uint8_t>& out) {
    out.reserve(out.size() + frame_size(channels, samples));
    put_u64(out, uint64_t(channels) << 56 | uint64_t(rate) << 32 | uint64_t(samples) << 16 |
                     uint64_t(frame_size(channels, samples)));
    for (int c = 0; c < channels; ++c) {
        uint64_t history = 0, weights = 0;
        for (int i = 0; i < 4; ++i) {
            // Continue from exactly what the decoder will read.
            lms[c].history[i] = int16_t(lms[c].history[i]);
            lms[c].weights[i] = int16_t(lms[c].weights[i]);
            history = (history << 16) | (uint32_t(lms[c].history[i]) & 0xffff);
            weights = (weights << 16) | (uint32_t(lms[c].weights[i]) & 0xffff);
        }
        put_u64(out, history);
        put_u64(out, weights);
    }

    int prev_sf[kMaxChannels] = {};
    for (int start = 0; start < samples; start += kSliceLen) {
        int len = std::min(kSliceLen, samples - start);
        for (int c = 0; c < channels; ++c) {
            // Try all 16 scalefactors (starting from the last slice's) and keep the
            // one with the least error.
            uint64_t best_rank = ~uint64_t(0), best_slice = 0;
            Lms best_lms = lms[c];
            int best_sf = 0;
            for (int i = 0; i < 16; ++i) {
                int sf = (i + prev_sf[c]) & 15;
                Lms l = lms[c];
                uint64_t slice = uint64_t(sf), rank = 0;
                int k = 0;
                for (; k < len; ++k) {
                    int32_t sample = interleaved[size_t(start + k) * channels + c];
                    int32_t predicted = predict(l);
                    int32_t residual = sample - predicted;
                    int32_t scaled = std::min(8, std::max(-8, div_sf(residual, sf)));
                    int quantized = kQuant[scaled + 8];
                    int32_t dequantized = kDequant[sf][quantized];
                    int32_t reconstructed = clamp_s16(predicted + dequantized);
                    // Penalize runaway weights; avoids pops in some problem signals.
                    int64_t wp = ((int64_t(l.weights[0]) * l.weights[0] + int64_t(l.weights[1]) * l.weights[1] +
                                   int64_t(l.weights[2]) * l.weights[2] + int64_t(l.weights[3]) * l.weights[3]) >>
                                  18) - 0x8ff;
                    if (wp < 0) wp = 0;
                    int64_t err = sample - reconstructed;
                    rank += uint64_t(err * err) + uint64_t(wp * wp);
                    if (rank > best_rank) break;
                    update(l, reconstructed, dequantized);
                    slice = (slice << 3) | uint64_t(quantized);
                }
                if (k == len && rank < best_rank) {
                    best_rank = rank;
                    best_slice = slice;
                    best_lms = l;
                    best_sf = sf;
                }
            }
            prev_sf[c] = best_sf;
            lms[c] = best_lms;
            put_u64(out, best_slice << ((kSliceLen - len) * 3));
        }
    }
}

int decode_frame(const uint8_t* data, size_t size, int channels, uint32_t rate, int16_t* out,
                 size_t* consumed) {
    size_t header_size = 8 + 16 * size_t(channels);
    if (size < header_size) return -1;
    uint64_t fh = get_u64(data);
    int fch = int(fh >> 56);
    uint32_t frate = uint32_t(fh >> 32) & 0xffffff;
    int samples = int(fh >> 16) & 0xffff;
    size_t fsize = size_t(fh & 0xffff);
    int slices = (samples + kSliceLen - 1) / kSliceLen;
    if (fch != channels || frate != rate || samples < 1 || slices > 256 || fsize > size ||
        fsize < header_size + 8 * size_t(slices) * size_t(channels))
        return -1;

    Lms lms[kMaxChannels];
    const uint8_t* p = data + 8;
    for (int c = 0; c < channels; ++c, p += 16) {
        uint64_t history = get_u64(p), weights = get_u64(p + 8);
        for (int i = 0; i < 4; ++i) {
            lms[c].history[i] = int16_t(history >> 48);
            lms[c].weights[i] = int16_t(weights >> 48);
            history <<= 16;
            weights <<= 16;
        }
    }
    for (int start = 0; start < samples; start += kSliceLen) {
        int len = std::min(kSliceLen, samples - start);
        for (int c = 0; c < channels; ++c, p += 8) {
            uint64_t slice = get_u64(p);
            int sf = int(slice >> 60);
            slice <<= 4;
            int16_t* o = out + size_t(start) * channels + c;
            for (int k = 0; k < len; ++k, o += channels) {
                int32_t predicted = predict(lms[c]);
                int32_t dequantized = kDequant[sf][int(slice >> 61)];
                int32_t reconstructed = clamp_s16(predicted + dequantized);
                *o = int16_t(reconstructed);
                slice <<= 3;
                update(lms[c], reconstructed, dequantized);
            }
        }
    }
    *consumed = fsize;
    return samples;
}

}  // namespace qoa

}  // namespace detail

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------
Status Decoder::read_header(ReadFn fn, void* user) {
    ready_ = false;
    uint8_t buf[kStreamHeaderSize];
    size_t n = read_full(fn, user, buf, kStreamHeaderSizeV1);
    if (n == 0) return Status::EndOfStream;
    if (n < size_t(kStreamHeaderSizeV1)) return Status::Truncated;
    if (buf[3] == '2') {
        n += read_full(fn, user, buf + n, size_t(kStreamHeaderSize - kStreamHeaderSizeV1));
        if (n < size_t(kStreamHeaderSize)) return Status::Truncated;
    }
    Status st = parse_stream_header(buf, n, &header_);
    if (st != Status::Ok) return st;
    if (uint64_t(header_.width) * header_.height > uint64_t(TJC_MAX_PIXELS)) return Status::BadHeader;

    layout_ = make_layout(header_.width, header_.height, header_.tile_w, header_.tile_h);
    uint16_t ql[64], qc[64];
    detail::build_quant_tables(header_.quality, ql, qc);
    for (int k = 0; k < 64; ++k) {
        dq_[0][k] = ql[detail::kZigzag[k]];
        dq_[1][k] = qc[detail::kZigzag[k]];
    }
    if (!fb_.alloc(layout_)) return Status::OutOfMemory;
    try {
        dirty_.assign(size_t(layout_.tiles), 0);
        bitmap_.assign(size_t(layout_.tiles + 7) / 8, 0);
        audio_.clear();
    } catch (...) {
        return Status::OutOfMemory;
    }
    audio_samples_ = 0;
    ready_ = true;
    return Status::Ok;
}

Status Decoder::decode_audio(ReadFn fn, void* user, size_t* bytes) {
    audio_samples_ = 0;
    uint8_t lb[4];
    if (read_full(fn, user, lb, 4) < 4) return Status::Truncated;
    size_t len = get_u32(lb);
    *bytes += 4 + len;
    // A frame never needs more than about one frame period of samples; allow plenty
    // of slack but refuse absurd sizes from damaged input.
    const size_t ch = header_.audio_channels;
    uint64_t expect = uint64_t(header_.audio_rate) * header_.fps_den / header_.fps_num + 1;
    uint64_t max_samples = 2 * expect + detail::qoa::kMaxFrameLen;
    if (len > detail::qoa::frame_size(int(ch), detail::qoa::kMaxFrameLen) * (max_samples / detail::qoa::kMaxFrameLen + 1))
        return Status::Corrupt;
    if (payload_.size() < len) {
        try {
            payload_.resize(len);
        } catch (...) {
            return Status::OutOfMemory;
        }
    }
    if (read_full(fn, user, payload_.data(), len) < len) return Status::Truncated;
    size_t pos = 0;
    while (pos < len) {
        if (audio_samples_ + detail::qoa::kMaxFrameLen > max_samples) return Status::Corrupt;
        size_t need = (audio_samples_ + detail::qoa::kMaxFrameLen) * ch;
        if (audio_.size() < need) {
            try {
                audio_.resize(need);
            } catch (...) {
                return Status::OutOfMemory;
            }
        }
        size_t used = 0;
        int got = detail::qoa::decode_frame(payload_.data() + pos, len - pos, int(ch), header_.audio_rate,
                                            audio_.data() + audio_samples_ * ch, &used);
        if (got < 0) return Status::Corrupt;
        audio_samples_ += size_t(got);
        pos += used;
    }
    return Status::Ok;
}

bool Decoder::decode_tile(const uint8_t* data, size_t size, int tile) {
    detail::BitReader br(data, size);
    int32_t coef[64];
    uint8_t blk[64];
    for (int c = 0; c < 3; ++c) {
        int x0, y0, w, h;
        tile_rect(layout_, tile, c, x0, y0, w, h);
        int stride = fb_.stride[c];
        int table = c ? 1 : 0;
        int pred = 0;
        for (int by = 0; by < h; by += 8) {
            for (int bx = 0; bx < w; bx += 8) {
                int r = detail::decode_block(br, table, dq_[table], pred, coef);
                if (r < 0) return false;
                uint8_t* dst = fb_.p[c].data() + size_t(y0 + by) * stride + x0 + bx;
                int bw = std::min(8, w - bx), bh = std::min(8, h - by);
                if (bw == 8 && bh == 8) {
                    if (r) detail::idct8x8(coef, dst, stride);
                    else detail::idct8x8_dc(coef[0], dst, stride);
                } else {
                    if (r) detail::idct8x8(coef, blk, 8);
                    else detail::idct8x8_dc(coef[0], blk, 8);
                    store_partial_block(blk, dst, stride, bw, bh);
                }
            }
        }
    }
    return !br.overrun();
}

Status Decoder::decode_frame(ReadFn fn, void* user, FrameStats* stats) {
    if (!ready_) return Status::NotInitialized;
    uint8_t hdr[kFrameHeaderSize];
    size_t n = read_full(fn, user, hdr, sizeof(hdr));
    if (n == 0) return Status::EndOfStream;
    if (n < sizeof(hdr)) return Status::Truncated;

    uint32_t frame_num = get_u32(hdr);
    if (hdr[4] > 1) return Status::Corrupt;
    bool force = hdr[4] != 0;
    size_t bytes = kFrameHeaderSize;
    if (force) {
        std::fill(dirty_.begin(), dirty_.end(), uint8_t(1));
    } else {
        if (read_full(fn, user, bitmap_.data(), bitmap_.size()) < bitmap_.size()) return Status::Truncated;
        detail::unpack_bitmap(bitmap_.data(), layout_.tiles, dirty_.data());
        bytes += bitmap_.size();
    }

    uint32_t dirty_count = 0;
    for (int t = 0; t < layout_.tiles; ++t) {
        if (!dirty_[size_t(t)]) continue;
        uint8_t lb[2];
        if (read_full(fn, user, lb, 2) < 2) return Status::Truncated;
        size_t len = get_u16(lb);
        if (len == 0) return Status::Corrupt;
        if (payload_.size() < len) {
            try {
                payload_.resize(len);
            } catch (...) {
                return Status::OutOfMemory;
            }
        }
        if (read_full(fn, user, payload_.data(), len) < len) return Status::Truncated;
        if (!decode_tile(payload_.data(), len, t)) return Status::Corrupt;
        bytes += 2 + len;
        ++dirty_count;
    }

    size_t video_bytes = bytes;
    if (header_.audio_codec) {
        Status st = decode_audio(fn, user, &bytes);
        if (st != Status::Ok) return st;
    }

    if (stats) {
        stats->frame_num = frame_num;
        stats->force_refresh = force;
        stats->dirty_tiles = dirty_count;
        stats->total_tiles = uint32_t(layout_.tiles);
        stats->bytes = bytes;
        stats->audio_samples = uint32_t(audio_samples_);
        stats->audio_bytes = header_.audio_codec ? bytes - video_bytes - 4 : 0;
        stats->audio_padded = 0;
    }
    return Status::Ok;
}

void Decoder::copy_frame(uint8_t* dst) const { copy_cropped(fb_, layout_, dst); }

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------
#ifndef TJC_NO_ENCODER
bool Encoder::init(const Config& cfg) {
    ready_ = false;
    if (const char* e = validate_geometry(cfg.width, cfg.height, cfg.tile_w, cfg.tile_h)) {
        error_ = e;
        return false;
    }
    if (cfg.quality < 1 || cfg.quality > 100) {
        error_ = "quality must be 1..100";
        return false;
    }
    if (cfg.refresh_param > 65535) {
        error_ = "refresh_param must be 0..65535";
        return false;
    }
    if (uint8_t(cfg.refresh_mode) > 2 || uint8_t(cfg.diff_reference) > 1) {
        error_ = "invalid refresh mode or diff reference";
        return false;
    }
    if (cfg.fps_num == 0 || cfg.fps_den == 0) {
        error_ = "frame rate numerator and denominator must be non-zero";
        return false;
    }
    if (cfg.audio_channels > detail::qoa::kMaxChannels ||
        (cfg.audio_channels && (cfg.audio_rate < 1 || cfg.audio_rate > 0xffffff))) {
        error_ = "audio needs 1..8 channels and a sample rate of 1..16777215 Hz";
        return false;
    }
    cfg_ = cfg;
    layout_ = make_layout(cfg.width, cfg.height, cfg.tile_w, cfg.tile_h);
    if (!cur_.alloc(layout_) || !last_coded_.alloc(layout_) || !recon_.alloc(layout_)) {
        error_ = "out of memory";
        return false;
    }
    dirty_.assign(size_t(layout_.tiles), 0);
    dirty_list_.clear();
    detail::build_quant_tables(cfg.quality, q_[0], q_[1]);
    for (int t = 0; t < 2; ++t) {
        for (int k = 0; k < 64; ++k) {
            qdiv_[t][k] = uint32_t(q_[t][detail::kZigzag[k]]) * 8;
            qrecip_[t][k] = detail::quant_recip(qdiv_[t][k]);
        }
    }
#ifdef TJC_NO_THREADS
    threads_ = 1;
#else
    threads_ = cfg.threads > 0 ? cfg.threads : int(std::thread::hardware_concurrency());
    threads_ = std::max(1, std::min(threads_, 256));
#endif
    worker_out_.assign(size_t(threads_), std::vector<uint8_t>());
    policy_.reset(cfg.refresh_mode, cfg.refresh_param, layout_.tiles);
    frame_num_ = 0;
    frame_index_ = 0;
    keyframe_request_ = false;
    audio_queue_.clear();
    audio_rem_ = 0;
    for (auto& l : audio_lms_) detail::qoa::init_lms(l);
    error_ = nullptr;
    ready_ = true;
    return true;
}

// Splits [0, n) into contiguous chunks, one per worker (fewer when there is too little
// work), and runs fn(begin, end, worker) on each. Chunk i always goes to worker i.
template <class F>
void Encoder::parallel_for(int n, int min_per_thread, F fn) {
    int workers = std::min(threads_, n / std::max(1, min_per_thread));
    if (workers <= 1) {
        if (n > 0) fn(0, n, 0);
        return;
    }
#ifdef TJC_NO_THREADS
    fn(0, n, 0);
#else
    std::thread pool[256];
    for (int w = 1; w < workers; ++w)
        pool[w] = std::thread(fn, int(int64_t(n) * w / workers), int(int64_t(n) * (w + 1) / workers), w);
    fn(0, int(int64_t(n) / workers), 0);
    for (int w = 1; w < workers; ++w) pool[w].join();
#endif
}

StreamHeader Encoder::stream_header() const {
    StreamHeader h;
    h.width = cfg_.width;
    h.height = cfg_.height;
    h.tile_w = cfg_.tile_w;
    h.tile_h = cfg_.tile_h;
    h.chroma_format = 0;
    h.refresh_mode = cfg_.refresh_mode;
    h.refresh_param = uint16_t(cfg_.refresh_param);
    h.quality = cfg_.quality;
    h.version = 2;
    h.fps_num = cfg_.fps_num;
    h.fps_den = cfg_.fps_den;
    h.audio_codec = cfg_.audio_channels ? 1 : 0;
    h.audio_channels = cfg_.audio_channels;
    h.audio_rate = cfg_.audio_rate;
    return h;
}

void Encoder::push_audio(const int16_t* interleaved, size_t samples_per_channel) {
    if (!cfg_.audio_channels) return;
    audio_queue_.insert(audio_queue_.end(), interleaved, interleaved + samples_per_channel * cfg_.audio_channels);
}

size_t Encoder::samples_for_next_frame() const {
    if (!cfg_.audio_channels) return 0;
    return size_t((audio_rem_ + uint64_t(cfg_.audio_rate) * cfg_.fps_den) / cfg_.fps_num);
}

void Encoder::write_stream_header(std::vector<uint8_t>& out) const {
    uint8_t buf[kStreamHeaderSize];
    tjc::write_stream_header(stream_header(), buf);
    out.insert(out.end(), buf, buf + kStreamHeaderSize);
}

void Encoder::load(const uint8_t* y, int ys, const uint8_t* u, int us, const uint8_t* v, int vs) {
    const uint8_t* src[3] = {y, u, v};
    int sstride[3] = {ys, us, vs};
    for (int c = 0; c < 3; ++c) {
        int w, h;
        visible_size(layout_, c, w, h);
        int pw = cur_.stride[c];
        int ph = c ? layout_.pcheight : layout_.pheight;
        uint8_t* d = cur_.p[c].data();
        for (int r = 0; r < h; ++r) {
            uint8_t* row = d + size_t(r) * pw;
            std::memcpy(row, src[c] + size_t(r) * sstride[c], size_t(w));
            std::memset(row + w, row[w - 1], size_t(pw - w));
        }
        for (int r = h; r < ph; ++r) std::memcpy(d + size_t(r) * pw, d + size_t(h - 1) * pw, size_t(pw));
    }
}

bool Encoder::tile_changed(int tile) const {
    // Judged per 8x8 block so a small moving object is not averaged away in a big tile.
    const Planes& ref = cfg_.diff_reference == DiffReference::Reconstructed ? recon_ : last_coded_;
    uint64_t k = cfg_.motion_threshold_k;
    for (int c = 0; c < 3; ++c) {
        int x, y, w, h;
        tile_rect(layout_, tile, c, x, y, w, h);
        int s = cur_.stride[c];
        for (int by = 0; by < h; by += 8) {
            for (int bx = 0; bx < w; bx += 8) {
                int bw = std::min(8, w - bx), bh = std::min(8, h - by);
                uint64_t limit = k * uint64_t(bw * bh);
                size_t off = size_t(y + by) * s + x + bx;
                if (detail::sad(cur_.p[c].data() + off, s, ref.p[c].data() + off, s, bw, bh, limit) > limit)
                    return true;
            }
        }
    }
    return false;
}

void Encoder::copy_tile(const Planes& from, Planes& to, int tile) {
    for (int c = 0; c < 3; ++c) {
        int x, y, w, h;
        tile_rect(layout_, tile, c, x, y, w, h);
        int s = from.stride[c];
        for (int r = 0; r < h; ++r) {
            size_t off = size_t(y + r) * s + x;
            std::memcpy(to.p[c].data() + off, from.p[c].data() + off, size_t(w));
        }
    }
}

void Encoder::encode_tile(int tile, std::vector<uint8_t>& out) {
    size_t len_pos = out.size();
    out.push_back(0);
    out.push_back(0);
    detail::BitWriter bw(out);
    int32_t dct[64], deq[64];
    int16_t zz[64];
    uint8_t blk[64];
    for (int c = 0; c < 3; ++c) {
        int x0, y0, w, h;
        tile_rect(layout_, tile, c, x0, y0, w, h);
        int stride = cur_.stride[c];
        int table = c ? 1 : 0;
        const uint16_t* q = q_[table];
        const uint32_t* qdiv = qdiv_[table];
        const uint64_t* qrecip = qrecip_[table];
        int pred = 0;
        for (int by = 0; by < h; by += 8) {
            for (int bx = 0; bx < w; bx += 8) {
                size_t off = size_t(y0 + by) * stride + x0 + bx;
                int bw_ = std::min(8, w - bx), bh_ = std::min(8, h - by);
                bool full = bw_ == 8 && bh_ == 8;
                if (full) {
                    detail::fdct8x8(cur_.p[c].data() + off, stride, dct);
                } else {
                    load_partial_block(cur_.p[c].data() + off, stride, bw_, bh_, blk);
                    detail::fdct8x8(blk, 8, dct);
                }
                int32_t ac_bits = 0;  // OR of the AC values, branch-free
                for (int k = 0; k < 64; ++k) {
                    int nat = detail::kZigzag[k];
                    int32_t qv = detail::quant_div(dct[nat], qdiv[k], qrecip[k]);
                    qv = std::min<int32_t>(1023, std::max<int32_t>(-1023, qv));
                    zz[k] = int16_t(qv);
                    // Shadow decode: same dequantization and clamp as decode_block().
                    deq[nat] = std::min<int32_t>(2047, std::max<int32_t>(-2048, qv * int32_t(q[nat])));
                    if (k) ac_bits |= qv;
                }
                bool has_ac = ac_bits != 0;
                detail::encode_block(bw, zz, pred, table);
                uint8_t* dst = full ? recon_.p[c].data() + off : blk;
                int dstride = full ? stride : 8;
                if (has_ac) detail::idct8x8(deq, dst, dstride);
                else detail::idct8x8_dc(deq[0], dst, dstride);
                if (!full) store_partial_block(blk, recon_.p[c].data() + off, stride, bw_, bh_);
            }
        }
    }
    bw.flush();
    put_u16(out.data() + len_pos, uint32_t(out.size() - len_pos - 2));
}

void Encoder::encode_frame(const uint8_t* yuv420p, std::vector<uint8_t>& out, FrameStats* stats) {
    size_t ys = size_t(cfg_.width) * cfg_.height;
    size_t cs = size_t(layout_.cwidth) * layout_.cheight;
    encode_frame(yuv420p, cfg_.width, yuv420p + ys, layout_.cwidth, yuv420p + ys + cs, layout_.cwidth, out,
                 stats);
}

void Encoder::encode_frame(const uint8_t* y, int ystride, const uint8_t* u, int ustride, const uint8_t* v,
                           int vstride, std::vector<uint8_t>& out, FrameStats* stats) {
    if (!ready_) return;
    size_t start = out.size();
    load(y, ystride, u, ustride, v, vstride);

    bool force = keyframe_request_ || policy_.forced(frame_index_);
    keyframe_request_ = false;
    int tiles = layout_.tiles;
    int tile_area = layout_.tile_w * layout_.tile_h;
    if (force) {
        std::fill(dirty_.begin(), dirty_.end(), uint8_t(1));
    } else {
        parallel_for(tiles, std::max(1, 32768 / tile_area), [this](int b, int e, int) {
            for (int t = b; t < e; ++t) dirty_[size_t(t)] = tile_changed(t) ? 1 : 0;
        });
        policy_.apply(dirty_.data());
    }
    dirty_list_.clear();
    for (int t = 0; t < tiles; ++t)
        if (dirty_[size_t(t)]) dirty_list_.push_back(t);

    uint8_t hdr[kFrameHeaderSize];
    put_u32(hdr, frame_num_);
    hdr[4] = force ? 1 : 0;
    out.insert(out.end(), hdr, hdr + kFrameHeaderSize);
    if (!force) {
        size_t pos = out.size();
        out.resize(pos + size_t(tiles + 7) / 8);
        detail::pack_bitmap(dirty_.data(), tiles, out.data() + pos);
    }

    // Tiles touch disjoint pixels, so they encode independently; each worker takes a
    // contiguous run of dirty tiles and the runs are joined in raster order, which
    // keeps the output identical for any thread count.
    for (auto& w : worker_out_) w.clear();
    parallel_for(int(dirty_list_.size()), std::max(1, 8192 / tile_area), [this](int b, int e, int worker) {
        std::vector<uint8_t>& wout = worker_out_[size_t(worker)];
        for (int i = b; i < e; ++i) {
            int t = dirty_list_[size_t(i)];
            encode_tile(t, wout);
            copy_tile(cur_, last_coded_, t);
        }
    });
    for (auto& w : worker_out_) out.insert(out.end(), w.begin(), w.end());
    uint32_t dirty_count = uint32_t(dirty_list_.size());

    // Audio for this frame: the samples up to floor((n+1) * rate * den / num).
    size_t audio_samples = 0, audio_padded = 0, audio_bytes = 0;
    if (cfg_.audio_channels) {
        const size_t ch = cfg_.audio_channels;
        uint64_t t = audio_rem_ + uint64_t(cfg_.audio_rate) * cfg_.fps_den;
        audio_samples = size_t(t / cfg_.fps_num);
        audio_rem_ = t % cfg_.fps_num;
        size_t have = audio_queue_.size() / ch;
        if (have < audio_samples) {
            audio_padded = audio_samples - have;
            audio_queue_.resize(audio_samples * ch, 0);
        }
        size_t len_pos = out.size();
        out.resize(len_pos + 4);
        for (size_t off = 0; off < audio_samples; off += size_t(detail::qoa::kMaxFrameLen)) {
            int n = int(std::min(audio_samples - off, size_t(detail::qoa::kMaxFrameLen)));
            detail::qoa::encode_frame(audio_queue_.data() + off * ch, int(ch), cfg_.audio_rate, n, audio_lms_, out);
        }
        audio_bytes = out.size() - len_pos - 4;
        put_u32(out.data() + len_pos, uint32_t(audio_bytes));
        audio_queue_.erase(audio_queue_.begin(), audio_queue_.begin() + std::ptrdiff_t(audio_samples * ch));
    }

    if (stats) {
        stats->frame_num = frame_num_;
        stats->force_refresh = force;
        stats->dirty_tiles = dirty_count;
        stats->total_tiles = uint32_t(tiles);
        stats->bytes = out.size() - start;
        stats->audio_samples = uint32_t(audio_samples);
        stats->audio_bytes = audio_bytes;
        stats->audio_padded = uint32_t(audio_padded);
    }
    ++frame_num_;
    ++frame_index_;
}

void Encoder::copy_recon(uint8_t* dst) const { copy_cropped(recon_, layout_, dst); }
#endif  // TJC_NO_ENCODER

}  // namespace tjc

#endif  // TJC_IMPLEMENTATION_DONE
#endif  // TJC_IMPLEMENTATION
