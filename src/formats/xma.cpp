#include <euphoria/formats/xma.hpp>

#include "xma_tables.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numbers>
#include <utility>

namespace euphoria::formats::xma {
namespace {

constexpr std::size_t kPacketBits = kPacketBytes * 8;
constexpr std::size_t kHeaderBits = 32;
constexpr std::uint32_t kLengthBits = 15;
constexpr std::size_t kMinSubframe = kSubframeSamples;
constexpr std::size_t kBlockSizes = 3;
constexpr std::size_t kMaxBands = 29;
constexpr std::size_t kMaxSubframes = 32;
constexpr std::size_t kOutSamples = kFrameSamples + kFrameSamples / 2;
constexpr std::size_t kTileIterations = 4096;

using Result = std::expected<void, std::string_view>;

std::unexpected<std::string_view> fail(std::string_view message) noexcept
{
    return std::unexpected<std::string_view>(message);
}

struct Cursor {
    std::span<const std::uint8_t> data;
    std::size_t pos;

    std::optional<std::uint32_t> bit() noexcept
    {
        if (pos % kPacketBits == 0) {
            const std::size_t previous = pos / kPacketBits;
            if (previous > 0) {
                const std::optional<Packet> header = packet(data, previous - 1);
                if (!header) {
                    return std::nullopt;
                }
                pos = (previous + header->skip) * kPacketBits;
            }
            pos += kHeaderBits;
        }
        if (pos / 8 >= data.size()) {
            return std::nullopt;
        }
        const std::uint32_t value = (data[pos / 8] >> (7 - pos % 8)) & 1u;
        ++pos;
        return value;
    }

    std::optional<std::uint32_t> bits(std::uint32_t count) noexcept
    {
        std::uint32_t value = 0;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::optional<std::uint32_t> next = bit();
            if (!next) {
                return std::nullopt;
            }
            value = (value << 1) | *next;
        }
        return value;
    }
};

std::optional<std::size_t> next_frame(std::span<const std::uint8_t> data, std::size_t start, std::size_t end)
{
    const std::size_t index = start / kPacketBits;
    const std::optional<Packet> header = packet(data, index);
    if (!header) {
        return std::nullopt;
    }
    std::size_t at = index * kPacketBits + kHeaderBits + header->first_frame_bit;
    std::uint32_t number = 0;
    while (at < start) {
        Cursor cursor{data, at};
        const std::optional<std::uint32_t> bits = cursor.bits(kLengthBits);
        if (!bits || *bits <= kLengthBits) {
            return std::nullopt;
        }
        at += *bits;
        ++number;
    }
    const bool last = number + 1 >= header->frames;
    if (end % kPacketBits == 0 || (last && end / kPacketBits == index)) {
        const std::size_t previous = (end - 1) / kPacketBits;
        const std::optional<Packet> ending = packet(data, previous);
        if (!ending) {
            return std::nullopt;
        }
        const std::size_t next = previous + 1 + ending->skip;
        const std::optional<Packet> following = packet(data, next);
        if (!following) {
            return std::nullopt;
        }
        end = next * kPacketBits + kHeaderBits + following->first_frame_bit;
    }
    if (end / 8 < data.size()) {
        return end;
    }
    return std::nullopt;
}

struct Bits {
    std::span<const std::uint8_t> data;
    std::size_t pos = 0;
    std::size_t end = 0;

    std::uint32_t peek32() const noexcept
    {
        std::uint64_t value = 0;
        const std::size_t byte = pos / 8;
        for (std::size_t i = 0; i < 5; ++i) {
            const std::uint64_t next = byte + i < data.size() ? data[byte + i] : 0u;
            value = (value << 8) | next;
        }
        return static_cast<std::uint32_t>((value << (24 + pos % 8)) >> 32);
    }

    std::uint32_t read(std::uint32_t count) noexcept
    {
        if (count == 0) {
            return 0;
        }
        const std::uint32_t value = peek32() >> (32 - count);
        pos += count;
        return value;
    }

    bool bit() noexcept
    {
        return read(1) != 0;
    }

    std::int32_t read_signed(std::uint32_t count) noexcept
    {
        return static_cast<std::int32_t>(read(count) << (32 - count)) >> (32 - count);
    }

    bool overread() const noexcept
    {
        return pos > end;
    }
};

struct VlcCode {
    std::uint32_t code;
    std::uint8_t length;
    std::int32_t symbol;
};

class Vlc {
public:
    void add(std::int32_t symbol, std::uint8_t length)
    {
        codes_.push_back(VlcCode{static_cast<std::uint32_t>(next_), length, symbol});
        next_ += std::uint64_t{1} << (32 - length);
    }

    std::int32_t read(Bits& bits) const noexcept
    {
        const std::uint32_t value = bits.peek32();
        auto it = std::upper_bound(codes_.begin(), codes_.end(), value,
                                   [](std::uint32_t v, const VlcCode& code) { return v < code.code; });
        --it;
        bits.pos += it->length;
        return it->symbol;
    }

private:
    std::vector<VlcCode> codes_;
    std::uint64_t next_ = 0;
};

struct Codes {
    Vlc scale;
    Vlc scale_rl;
    std::array<Vlc, 2> coef;
    Vlc vec4;
    Vlc vec2;
    Vlc vec1;
};

template <std::size_t N>
Vlc from_pairs(const std::array<tables::Code, N>& table, std::int32_t offset)
{
    Vlc vlc;
    for (const tables::Code& entry : table) {
        vlc.add(static_cast<std::int32_t>(entry.symbol) + offset, entry.length);
    }
    return vlc;
}

template <std::size_t N>
Vlc from_columns(const std::array<std::uint16_t, N>& symbols, const std::array<std::uint8_t, N>& lengths,
                 std::int32_t offset)
{
    Vlc vlc;
    for (std::size_t i = 0; i < N; ++i) {
        vlc.add(static_cast<std::int32_t>(symbols[i]) + offset, lengths[i]);
    }
    return vlc;
}

const Codes& codes()
{
    static const Codes instance{
        from_pairs(tables::kScale, -60),
        from_pairs(tables::kScaleRl, 0),
        {from_columns(tables::kCoef0Syms, tables::kCoef0Lens, 0), from_pairs(tables::kCoef1, 0)},
        from_columns(tables::kVec4Syms, tables::kVec4Lens, -1),
        from_pairs(tables::kVec2, -1),
        from_pairs(tables::kVec1, 0),
    };
    return instance;
}

std::uint32_t large_value(Bits& bits) noexcept
{
    std::uint32_t count = 8;
    if (bits.bit()) {
        count += 8;
        if (bits.bit()) {
            count += 8;
            if (bits.bit()) {
                count += 7;
            }
        }
    }
    return bits.read(count);
}

std::uint32_t ilog2(std::size_t value) noexcept
{
    return static_cast<std::uint32_t>(std::bit_width(std::max<std::size_t>(value, 1)) - 1);
}

void fft(double* re, double* im, std::size_t n) noexcept
{
    std::size_t j = 0;
    for (std::size_t i = 1; i < n; ++i) {
        std::size_t bit = n >> 1;
        while ((j & bit) != 0) {
            j ^= bit;
            bit >>= 1;
        }
        j |= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (std::size_t length = 2; length <= n; length <<= 1) {
        const double angle = -2.0 * std::numbers::pi / static_cast<double>(length);
        const double ws = std::sin(angle);
        const double wc = std::cos(angle);
        for (std::size_t start = 0; start < n; start += length) {
            double cr = 1.0;
            double ci = 0.0;
            for (std::size_t k = 0; k < length / 2; ++k) {
                const std::size_t a = start + k;
                const std::size_t b = start + k + length / 2;
                const double tr = re[b] * cr - im[b] * ci;
                const double ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                const double next_r = cr * wc - ci * ws;
                ci = cr * ws + ci * wc;
                cr = next_r;
            }
        }
    }
}

void imdct_half(const float* input, std::size_t n, float* out, float scale) noexcept
{
    const std::size_t m = n / 2;
    std::array<double, kFrameSamples / 2> re{};
    std::array<double, kFrameSamples / 2> im{};
    const double size = static_cast<double>(n);
    for (std::size_t k = 0; k < m; ++k) {
        const double a = input[2 * k];
        const double b = input[n - 1 - 2 * k];
        const double angle = -std::numbers::pi * static_cast<double>(k) / size;
        const double s = std::sin(angle);
        const double c = std::cos(angle);
        re[k] = a * c - b * s;
        im[k] = a * s + b * c;
    }
    fft(re.data(), im.data(), m);
    for (std::size_t k = 0; k < m; ++k) {
        const double angle = -std::numbers::pi * static_cast<double>(4 * k + 1) / (4.0 * size);
        const double s = std::sin(angle);
        const double c = std::cos(angle);
        const double wr = re[k] * c - im[k] * s;
        const double wi = re[k] * s + im[k] * c;
        out[n - 1 - 2 * k] = static_cast<float>(wr) * scale;
        out[2 * k] = -static_cast<float>(wi) * scale;
    }
}

std::vector<float> sine_window(std::size_t n)
{
    std::vector<float> window(n);
    for (std::size_t i = 0; i < n; ++i) {
        window[i] = static_cast<float>(
            std::sin((static_cast<double>(i) + 0.5) * std::numbers::pi / (2.0 * static_cast<double>(n))));
    }
    return window;
}

const std::array<std::vector<float>, 3>& windows()
{
    static const std::array<std::vector<float>, 3> instance{sine_window(128), sine_window(256), sine_window(512)};
    return instance;
}

void window_overlap(float* block, std::size_t size, const std::vector<float>& window) noexcept
{
    const std::size_t half = size / 2;
    for (std::size_t k = 0; k < half; ++k) {
        const std::size_t i = k;
        const std::size_t j = 2 * half - 1 - k;
        const float s0 = block[i];
        const float s1 = block[j];
        const float wi = window[i];
        const float wj = window[j];
        block[i] = s0 * wj - s1 * wi;
        block[j] = s0 * wi + s1 * wj;
    }
}

std::int16_t to_i16(float value) noexcept
{
    return static_cast<std::int16_t>(std::clamp(std::round(value * 32768.0f), -32768.0f, 32767.0f));
}

struct Channel {
    std::size_t prev_block_len = kFrameSamples;
    bool transmit_coefs = false;
    std::size_t num_subframes = 0;
    std::array<std::size_t, kMaxSubframes> subframe_len{};
    std::size_t cur_subframe = 0;
    std::size_t decoded_samples = 0;
    std::int32_t quant_step = 0;
    bool reuse_sf = false;
    std::int32_t scale_factor_step = 1;
    std::int32_t max_scale_factor = 0;
    std::array<std::array<std::int32_t, kMaxBands>, 2> saved_scale_factors{};
    std::size_t scale_factor_idx = 0;
    std::size_t scale_factors = 0;
    std::size_t table_idx = 0;
    std::size_t num_vec_coeffs = 0;
    std::array<float, kFrameSamples> coeffs{};
    std::array<float, kOutSamples> out{};
};

}

struct Decoder::State {
    std::size_t channels = 1;
    std::array<std::size_t, kBlockSizes> num_sfb{};
    std::array<std::array<std::size_t, kMaxBands + 1>, kBlockSizes> sfb_offsets{};
    std::array<std::array<std::array<std::size_t, kMaxBands>, kBlockSizes>, kBlockSizes> sf_offsets{};
    std::array<Channel, 2> ch{};
    std::size_t subframe_len = kFrameSamples;
    std::size_t channels_for_cur_subframe = 0;
    std::array<std::size_t, 2> channel_indexes{0, 1};
    std::size_t num_bands = 0;
    std::size_t table_idx = 0;
    std::uint32_t esc_len = 0;
    bool transmit_num_vec_coeffs = false;
    bool transform = false;
    std::array<bool, kMaxBands> transform_band{};
    bool parsed_all_subframes = false;

    Result frame(Bits& b, std::size_t length);
    std::expected<std::size_t, std::string_view> subframe_length(Bits& b, std::size_t offset);
    Result tile_header(Bits& b);
    Result channel_transform(Bits& b);
    Result coefficients(Bits& b, std::size_t c);
    Result scale_factors(Bits& b);
    Result subframe(Bits& b);
    void inverse_channel_transform();
};

Result Decoder::State::frame(Bits& b, std::size_t length)
{
    if (Result r = tile_header(b); !r) {
        return r;
    }
    if (channels > 1 && b.bit() && b.bit()) {
        b.read(static_cast<std::uint32_t>(channels * channels * 4));
    }
    b.read(8);
    if (b.bit()) {
        if (b.bit()) {
            b.read(ilog2(kFrameSamples * 2));
        }
        if (b.bit()) {
            b.read(ilog2(kFrameSamples * 2));
        }
    }
    parsed_all_subframes = false;
    for (std::size_t c = 0; c < channels; ++c) {
        ch[c].decoded_samples = 0;
        ch[c].cur_subframe = 0;
        ch[c].reuse_sf = false;
    }
    while (!parsed_all_subframes) {
        if (Result r = subframe(b); !r) {
            return r;
        }
    }
    if (b.overread() || b.pos > length) {
        return fail("frame overread");
    }
    return {};
}

std::expected<std::size_t, std::string_view> Decoder::State::subframe_length(Bits& b, std::size_t offset)
{
    if (offset == kFrameSamples - kMinSubframe) {
        return kMinSubframe;
    }
    const std::uint32_t shift = b.bit() ? 1 + b.read(1) : 0;
    const std::size_t length = kFrameSamples >> shift;
    if (length < kMinSubframe) {
        return fail("subframe length");
    }
    return length;
}

Result Decoder::State::tile_header(Bits& b)
{
    std::array<std::size_t, 2> num_samples{};
    std::array<bool, 2> contains{};
    std::size_t channels_for_cur = channels;
    std::size_t min_channel_len = 0;
    for (std::size_t c = 0; c < channels; ++c) {
        ch[c].num_subframes = 0;
    }
    const bool fixed = b.bit();
    for (std::size_t iteration = 0;; ++iteration) {
        if (iteration >= kTileIterations) {
            return fail("tile header loops");
        }
        for (std::size_t c = 0; c < channels; ++c) {
            contains[c] = num_samples[c] == min_channel_len
                && (fixed || channels_for_cur == 1 || min_channel_len == kFrameSamples - kMinSubframe || b.bit());
        }
        const auto length = subframe_length(b, min_channel_len);
        if (!length) {
            return fail(length.error());
        }
        min_channel_len += *length;
        for (std::size_t c = 0; c < channels; ++c) {
            Channel& channel = ch[c];
            if (contains[c]) {
                if (channel.num_subframes >= kMaxSubframes) {
                    return fail("too many subframes");
                }
                channel.subframe_len[channel.num_subframes] = *length;
                num_samples[c] += *length;
                ++channel.num_subframes;
                if (num_samples[c] > kFrameSamples) {
                    return fail("channel longer than a frame");
                }
            } else if (num_samples[c] <= min_channel_len) {
                if (num_samples[c] < min_channel_len) {
                    channels_for_cur = 0;
                    min_channel_len = num_samples[c];
                }
                ++channels_for_cur;
            }
        }
        if (min_channel_len >= kFrameSamples || b.overread()) {
            break;
        }
    }
    if (b.overread()) {
        return fail("tile header");
    }
    return {};
}

Result Decoder::State::channel_transform(Bits& b)
{
    transform = false;
    if (channels == 1) {
        return {};
    }
    if (b.bit()) {
        return fail("channel transform bit");
    }
    if (channels_for_cur_subframe == 2) {
        if (b.bit()) {
            if (b.bit()) {
                return fail("channel transform type");
            }
        } else {
            transform = true;
        }
    }
    if (transform) {
        if (!b.bit()) {
            for (std::size_t i = 0; i < num_bands; ++i) {
                transform_band[i] = b.bit();
            }
        } else {
            transform_band.fill(true);
        }
    }
    return {};
}

Result Decoder::State::coefficients(Bits& b, std::size_t c)
{
    const Codes& vlc = codes();
    const std::size_t table = b.bit() ? 1 : 0;
    const std::uint16_t* run = table == 1 ? tables::kCoef1Run.data() : tables::kCoef0Run.data();
    const float* level = table == 1 ? tables::kCoef1Level.data() : tables::kCoef0Level.data();
    const std::size_t run_size = table == 1 ? tables::kCoef1Run.size() : tables::kCoef0Run.size();
    const std::size_t length = subframe_len;
    Channel& channel = ch[c];
    bool rl_mode = false;
    std::size_t cur = 0;
    std::size_t num_zeros = 0;
    while ((transmit_num_vec_coeffs || !rl_mode) && cur + 3 < channel.num_vec_coeffs) {
        std::array<std::uint32_t, 4> values{};
        const std::int32_t idx = vlc.vec4.read(b);
        if (idx < 0) {
            for (std::size_t i = 0; i < 4; i += 2) {
                const std::int32_t pair = vlc.vec2.read(b);
                if (pair < 0) {
                    for (std::size_t k = i; k < i + 2; ++k) {
                        std::uint32_t x = static_cast<std::uint32_t>(vlc.vec1.read(b));
                        if (x == tables::kVec1.size() - 1) {
                            x += large_value(b);
                        }
                        values[k] = x;
                    }
                } else {
                    values[i] = static_cast<std::uint32_t>(pair >> 4);
                    values[i + 1] = static_cast<std::uint32_t>(pair & 0xf);
                }
            }
        } else {
            values = {static_cast<std::uint32_t>(idx >> 12), static_cast<std::uint32_t>((idx >> 8) & 0xf),
                      static_cast<std::uint32_t>((idx >> 4) & 0xf), static_cast<std::uint32_t>(idx & 0xf)};
        }
        for (const std::uint32_t v : values) {
            if (v != 0) {
                const bool negative = !b.bit();
                const float magnitude = static_cast<float>(v);
                channel.coeffs[cur] = negative ? -magnitude : magnitude;
                num_zeros = 0;
            } else {
                channel.coeffs[cur] = 0.0f;
                ++num_zeros;
                rl_mode = rl_mode || num_zeros > (length >> 8);
            }
            ++cur;
        }
        if (b.overread()) {
            return fail("vector coefficients");
        }
    }
    if (cur < length) {
        std::fill(channel.coeffs.begin() + static_cast<std::ptrdiff_t>(cur),
                  channel.coeffs.begin() + static_cast<std::ptrdiff_t>(length), 0.0f);
        const std::size_t mask = length - 1;
        std::size_t offset = cur;
        while (offset < length) {
            const std::int32_t code = vlc.coef[table].read(b);
            if (code > 1) {
                const std::size_t index = static_cast<std::size_t>(code) < run_size ? static_cast<std::size_t>(code) : 0;
                offset += run[index];
                const bool negative = !b.bit();
                const float v = level[index];
                channel.coeffs[offset & mask] = negative ? -v : v;
            } else if (code == 1) {
                break;
            } else {
                const std::int32_t value = static_cast<std::int32_t>(large_value(b));
                if (b.bit()) {
                    if (b.bit()) {
                        if (b.bit()) {
                            break;
                        }
                        offset += b.read(esc_len) + 4;
                    } else {
                        offset += b.read(2) + 1;
                    }
                }
                const bool negative = !b.bit();
                channel.coeffs[offset & mask] = static_cast<float>(negative ? -value : value);
            }
            if (b.overread()) {
                return fail("run level coefficients");
            }
            ++offset;
        }
    }
    return {};
}

Result Decoder::State::scale_factors(Bits& b)
{
    const Codes& vlc = codes();
    for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
        const std::size_t c = channel_indexes[i];
        const std::array<std::size_t, kMaxBands> offsets = sf_offsets[table_idx][ch[c].table_idx];
        Channel& channel = ch[c];
        const std::size_t target = 1 - channel.scale_factor_idx;
        channel.scale_factors = target;
        if (channel.reuse_sf) {
            const std::array<std::int32_t, kMaxBands> from = channel.saved_scale_factors[channel.scale_factor_idx];
            for (std::size_t band = 0; band < num_bands; ++band) {
                channel.saved_scale_factors[target][band] = from[offsets[band]];
            }
        }
        if (channel.cur_subframe == 0 || b.bit()) {
            if (!channel.reuse_sf) {
                channel.scale_factor_step = static_cast<std::int32_t>(b.read(2)) + 1;
                std::int32_t value = 45 / channel.scale_factor_step;
                for (std::size_t band = 0; band < num_bands; ++band) {
                    value += vlc.scale.read(b);
                    channel.saved_scale_factors[target][band] = value;
                }
            } else {
                std::size_t band = 0;
                while (band < num_bands) {
                    const std::int32_t idx = vlc.scale_rl.read(b);
                    std::uint32_t skip = 0;
                    std::int32_t value = 0;
                    bool negative = false;
                    if (idx == 0) {
                        const std::uint32_t code = b.read(14);
                        skip = (code & 0x3f) >> 1;
                        value = static_cast<std::int32_t>(code >> 6);
                        negative = (code & 1) == 0;
                    } else if (idx == 1) {
                        break;
                    } else {
                        skip = tables::kScaleRlRun[static_cast<std::size_t>(idx)];
                        value = tables::kScaleRlLevel[static_cast<std::size_t>(idx)];
                        negative = !b.bit();
                    }
                    band += skip;
                    if (band >= num_bands) {
                        return fail("scale factor run");
                    }
                    channel.saved_scale_factors[target][band] += negative ? -value : value;
                    ++band;
                }
            }
            channel.scale_factor_idx = target;
            channel.table_idx = table_idx;
            channel.reuse_sf = true;
        }
        std::int32_t maximum = 0;
        for (std::size_t band = 0; band < num_bands; ++band) {
            const std::int32_t value = channel.saved_scale_factors[channel.scale_factors][band];
            maximum = band == 0 ? value : std::max(maximum, value);
        }
        channel.max_scale_factor = maximum;
    }
    return {};
}

Result Decoder::State::subframe(Bits& b)
{
    std::size_t offset = kFrameSamples;
    std::size_t length = kFrameSamples;
    std::int64_t total_samples = static_cast<std::int64_t>(kFrameSamples * channels);
    for (std::size_t c = 0; c < channels; ++c) {
        const Channel& channel = ch[c];
        if (offset > channel.decoded_samples) {
            offset = channel.decoded_samples;
            length = channel.subframe_len[std::min(channel.cur_subframe, kMaxSubframes - 1)];
        }
    }
    channels_for_cur_subframe = 0;
    for (std::size_t c = 0; c < channels; ++c) {
        Channel& channel = ch[c];
        total_samples -= static_cast<std::int64_t>(channel.decoded_samples);
        if (offset == channel.decoded_samples && channel.cur_subframe < channel.num_subframes
            && length == channel.subframe_len[channel.cur_subframe]) {
            total_samples -= static_cast<std::int64_t>(length);
            channel.decoded_samples += length;
            channel_indexes[channels_for_cur_subframe] = c;
            ++channels_for_cur_subframe;
        }
    }
    if (channels_for_cur_subframe == 0) {
        return fail("no channel in subframe");
    }
    if (length < kMinSubframe || length > kFrameSamples) {
        return fail("subframe length");
    }
    if (total_samples == 0) {
        parsed_all_subframes = true;
    }
    table_idx = ilog2(kFrameSamples / length);
    num_bands = num_sfb[table_idx];
    subframe_len = length;
    esc_len = ilog2(length - 1) + 1;

    if (b.bit()) {
        std::size_t fill = b.read(2);
        if (fill == 0) {
            const std::uint32_t bits = b.read(4);
            fill = static_cast<std::size_t>(b.read(bits)) + 1;
        }
        b.pos += fill;
    }
    if (b.bit()) {
        return fail("reserved bit");
    }
    if (Result r = channel_transform(b); !r) {
        return r;
    }

    bool transmit_coeffs = false;
    for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
        Channel& channel = ch[channel_indexes[i]];
        channel.transmit_coefs = b.bit();
        transmit_coeffs = transmit_coeffs || channel.transmit_coefs;
    }
    if (transmit_coeffs) {
        transmit_num_vec_coeffs = b.bit();
        if (transmit_num_vec_coeffs) {
            const std::uint32_t num_bits = ilog2((length + 3) / 4) + 1;
            for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
                const std::size_t count = static_cast<std::size_t>(b.read(num_bits)) << 2;
                if (count > length) {
                    return fail("vector coefficient count");
                }
                ch[channel_indexes[i]].num_vec_coeffs = count;
            }
        } else {
            for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
                ch[channel_indexes[i]].num_vec_coeffs = length;
            }
        }
        std::int32_t quant_step = 90;
        std::int32_t step = b.read_signed(6);
        quant_step += step;
        if (step == -32 || step == 31) {
            const bool negative = step == -32;
            std::int32_t quant = 0;
            while (b.pos + 5 < b.end) {
                step = static_cast<std::int32_t>(b.read(5));
                if (step != 31) {
                    break;
                }
                quant += 31;
            }
            quant_step += negative ? -(quant + step) : quant + step;
        }
        if (channels_for_cur_subframe == 1) {
            ch[channel_indexes[0]].quant_step = quant_step;
        } else {
            const std::uint32_t modifier_len = b.read(3);
            for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
                Channel& channel = ch[channel_indexes[i]];
                channel.quant_step = quant_step;
                if (b.bit()) {
                    channel.quant_step += modifier_len > 0 ? static_cast<std::int32_t>(b.read(modifier_len)) + 1 : 1;
                }
            }
        }
        if (Result r = scale_factors(b); !r) {
            return r;
        }
    }

    for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
        const std::size_t c = channel_indexes[i];
        if (ch[c].transmit_coefs && b.pos < b.end) {
            if (Result r = coefficients(b, c); !r) {
                return r;
            }
        } else {
            std::fill(ch[c].coeffs.begin(), ch[c].coeffs.begin() + static_cast<std::ptrdiff_t>(length), 0.0f);
        }
    }

    const std::size_t at = offset + kFrameSamples / 2;
    if (transmit_coeffs) {
        inverse_channel_transform();
        const float scale = 2.0f / static_cast<float>(length) / 32768.0f;
        const std::array<std::size_t, kMaxBands + 1>& sfb = sfb_offsets[table_idx];
        std::array<float, kFrameSamples> tmp{};
        for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
            Channel& channel = ch[channel_indexes[i]];
            const std::array<std::int32_t, kMaxBands>& sf = channel.saved_scale_factors[channel.scale_factors];
            for (std::size_t band = 0; band < num_bands; ++band) {
                const std::size_t end = std::min(sfb[band + 1], length);
                const std::int32_t exponent =
                    channel.quant_step - (channel.max_scale_factor - sf[band]) * channel.scale_factor_step;
                const float quant = std::pow(10.0f, static_cast<float>(exponent) / 20.0f);
                for (std::size_t k = sfb[band]; k < end; ++k) {
                    tmp[k] = channel.coeffs[k] * quant;
                }
            }
            imdct_half(tmp.data(), length, channel.out.data() + at, scale);
        }
    } else {
        for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
            Channel& channel = ch[channel_indexes[i]];
            std::fill(channel.out.begin() + static_cast<std::ptrdiff_t>(at),
                      channel.out.begin() + static_cast<std::ptrdiff_t>(at + length), 0.0f);
        }
    }

    for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
        Channel& channel = ch[channel_indexes[i]];
        std::size_t window_len = channel.prev_block_len;
        std::size_t start = at - window_len / 2;
        if (length < window_len) {
            start += (window_len - length) / 2;
            window_len = length;
        }
        window_overlap(channel.out.data() + start, window_len, windows()[ilog2(window_len) - 7]);
        channel.prev_block_len = length;
    }

    for (std::size_t i = 0; i < channels_for_cur_subframe; ++i) {
        Channel& channel = ch[channel_indexes[i]];
        if (channel.cur_subframe >= channel.num_subframes) {
            return fail("subframe overrun");
        }
        ++channel.cur_subframe;
    }
    if (b.overread()) {
        return fail("subframe overread");
    }
    return {};
}

void Decoder::State::inverse_channel_transform()
{
    if (!transform || channels_for_cur_subframe != 2) {
        return;
    }
    const std::array<std::size_t, kMaxBands + 1>& sfb = sfb_offsets[table_idx];
    Channel& a = ch[0];
    Channel& b = ch[1];
    for (std::size_t band = 0; band < num_bands; ++band) {
        const std::size_t end = std::min(sfb[band + 1], subframe_len);
        if (transform_band[band]) {
            for (std::size_t y = sfb[band]; y < end; ++y) {
                const float l = a.coeffs[y];
                const float r = b.coeffs[y];
                a.coeffs[y] = l - r;
                b.coeffs[y] = l + r;
            }
        } else {
            for (std::size_t y = sfb[band]; y < end; ++y) {
                a.coeffs[y] *= 181.0f / 128.0f;
                b.coeffs[y] *= 181.0f / 128.0f;
            }
        }
    }
}

std::optional<Packet> packet(std::span<const std::uint8_t> data, std::size_t index) noexcept
{
    if (index > data.size() / kPacketBytes) {
        return std::nullopt;
    }
    const std::size_t start = index * kPacketBytes;
    if (data.size() - start < 4) {
        return std::nullopt;
    }
    const std::uint32_t header = (std::uint32_t{data[start]} << 24) | (std::uint32_t{data[start + 1]} << 16)
        | (std::uint32_t{data[start + 2]} << 8) | std::uint32_t{data[start + 3]};
    return Packet{header >> 26, (header >> 11) & 0x7fffu, header & 0xffu};
}

std::optional<Frame> read_frame(std::span<const std::uint8_t> data, std::size_t start)
{
    Cursor probe{data, start};
    const std::optional<std::uint32_t> length = probe.bits(kLengthBits);
    if (!length || *length <= kLengthBits || *length == 0x7fff) {
        return std::nullopt;
    }
    const std::size_t bits = *length;
    Frame frame{std::vector<std::uint8_t>((bits + 7) / 8), bits, start, std::nullopt};
    Cursor cursor{data, start};
    for (std::size_t i = 0; i < bits; ++i) {
        const std::optional<std::uint32_t> bit = cursor.bit();
        if (!bit) {
            return std::nullopt;
        }
        frame.data[i / 8] |= static_cast<std::uint8_t>(*bit << (7 - i % 8));
    }
    frame.next = next_frame(data, start, cursor.pos);
    return frame;
}

Decoder::Decoder(std::size_t channels, std::uint32_t sample_rate) : state_(std::make_unique<State>())
{
    State& s = *state_;
    s.channels = std::clamp<std::size_t>(channels, 1, 2);
    std::size_t rate = 24000;
    if (sample_rate > 44100) {
        rate = 48000;
    } else if (sample_rate > 32000) {
        rate = 44100;
    } else if (sample_rate > 24000) {
        rate = 32000;
    }
    for (std::size_t i = 0; i < kBlockSizes; ++i) {
        const std::size_t subframe_len = kFrameSamples >> i;
        std::array<std::size_t, kMaxBands + 1>& offsets = s.sfb_offsets[i];
        std::size_t band = 1;
        for (std::size_t f = 0; f < kMaxBands - 1 && f < tables::kCriticalFreq.size(); ++f) {
            if (offsets[band - 1] >= subframe_len) {
                break;
            }
            const std::size_t offset = ((subframe_len * 2 * tables::kCriticalFreq[f]) / rate + 2) & ~std::size_t{3};
            if (offset > offsets[band - 1]) {
                offsets[band] = offset;
                ++band;
            }
            if (offset >= subframe_len) {
                break;
            }
        }
        offsets[band - 1] = subframe_len;
        s.num_sfb[i] = band - 1;
    }
    for (std::size_t i = 0; i < kBlockSizes; ++i) {
        for (std::size_t b = 0; b < s.num_sfb[i]; ++b) {
            const std::size_t offset = ((s.sfb_offsets[i][b] + s.sfb_offsets[i][b + 1] - 1) << i) >> 1;
            for (std::size_t x = 0; x < kBlockSizes; ++x) {
                std::size_t v = 0;
                while (v + 1 < kMaxBands && (s.sfb_offsets[x][v + 1] << x) < offset) {
                    ++v;
                }
                s.sf_offsets[i][x][b] = v;
            }
        }
    }
}

Decoder::~Decoder() = default;
Decoder::Decoder(Decoder&&) noexcept = default;
Decoder& Decoder::operator=(Decoder&&) noexcept = default;

std::size_t Decoder::channels() const noexcept
{
    return state_->channels;
}

void Decoder::reset()
{
    state_->ch = {};
}

std::expected<void, std::string_view> Decoder::decode(std::span<const std::uint8_t> data, std::size_t bits,
                                                      std::vector<std::int16_t>& out)
{
    State& s = *state_;
    Bits b{data, 0, bits};
    const std::size_t length = b.read(kLengthBits);
    Result result = s.frame(b, length);
    if (!result) {
        for (std::size_t c = 0; c < s.channels; ++c) {
            s.ch[c].out.fill(0.0f);
        }
    }
    for (std::size_t i = 0; i < kFrameSamples; ++i) {
        for (std::size_t c = 0; c < s.channels; ++c) {
            out.push_back(to_i16(s.ch[c].out[i]));
        }
    }
    for (std::size_t c = 0; c < s.channels; ++c) {
        std::copy(s.ch[c].out.begin() + kFrameSamples, s.ch[c].out.end(), s.ch[c].out.begin());
    }
    return result;
}

void Decoder::flush(std::vector<std::int16_t>& out)
{
    State& s = *state_;
    for (std::size_t i = 0; i < kFrameSamples / 2; ++i) {
        for (std::size_t c = 0; c < s.channels; ++c) {
            out.push_back(to_i16(s.ch[c].out[i]));
        }
    }
    reset();
}

}
