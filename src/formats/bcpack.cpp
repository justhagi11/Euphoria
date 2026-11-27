#include <euphoria/formats/bcpack.hpp>

#include <algorithm>
#include <array>
#include <deque>
#include <format>
#include <utility>
#include <vector>

namespace euphoria::formats::bcpack {

namespace {

constexpr std::size_t kModels = 128;
constexpr std::size_t kBuckets = 9;
constexpr std::size_t kHistory = 256;
constexpr std::size_t kDictionary = 255;
constexpr std::uint32_t kLow = 1u << 24;
constexpr int kRoundBytes = 8;

static_assert(tables_size
              == kModels * kBuckets * 2 + 256 + 64 + 256 + 64 + 64 + 512 * 2
                     + kDictionary * (6 + 16 + 8 + 32 + 2 + 16 + 4 + 32 + 25 + 33 + 50 + 66));

struct Plain {
    std::array<std::uint8_t, 8> ep{};
    std::array<std::uint8_t, 32> ix{};
};

struct Bc6Ep {
    std::array<std::array<std::uint16_t, 3>, 4> e{};
};

struct Bc6Ix {
    std::uint8_t mode{};
    std::array<std::int16_t, 16> ix{};
};

struct Bc6Entry {
    Bc6Ep ep;
    Bc6Ix ix;
};

struct Bc7Ep {
    std::uint16_t ctrl{};
    std::array<std::array<std::uint16_t, 8>, 3> rows{};
};

struct Bc7Ix {
    std::array<std::uint8_t, 2> bits{};
    std::array<std::array<std::int16_t, 16>, 2> ix{};
};

struct Bc7Entry {
    Bc7Ep ep;
    Bc7Ix ix;
};

using Error = std::string_view;

}

struct Tables {
    std::array<std::array<std::uint32_t, kBuckets>, kModels> freq{};
    std::array<std::array<std::uint32_t, kBuckets + 1>, kModels> start{};
    std::array<std::array<std::uint8_t, 256>, kModels> bucket{};
    std::array<std::array<std::uint64_t, kBuckets>, kModels> recip{};
    std::array<std::uint8_t, 256> rank1{};
    std::array<std::uint8_t, 64> rank3a{};
    std::array<std::uint8_t, 256> rank3c{};
    std::array<std::uint8_t, 64> rank4{};
    std::array<std::uint8_t, 64> rank5{};
    std::array<std::uint16_t, 512> ctrl7{};
    std::vector<Plain> bc1;
    std::vector<Plain> bc3;
    std::vector<Plain> bc4;
    std::vector<Plain> bc5;
    std::vector<Bc6Entry> bc6;
    std::vector<Bc7Entry> bc7;
};

void TablesDelete::operator()(Tables* tables) const noexcept
{
    delete tables;
}

namespace {

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

    std::uint8_t u8() noexcept
    {
        if (at_ >= bytes_.size()) {
            return 0;
        }
        return bytes_[at_++];
    }

    std::uint16_t u16() noexcept
    {
        const std::uint16_t low = u8();
        const std::uint16_t high = u8();
        return static_cast<std::uint16_t>(low | (high << 8));
    }

    template <std::size_t N>
    void fill(std::array<std::uint8_t, N>& out) noexcept
    {
        for (auto& value : out) {
            value = u8();
        }
    }

private:
    std::span<const std::uint8_t> bytes_;
    std::size_t at_{};
};

struct Lane {
    std::uint8_t width{};
    std::uint8_t phase{};
    bool long_length{};
    bool reset{};
    std::uint8_t model{};
    bool need{};
    std::int16_t latch{-1};
    std::int16_t out{-1};
    bool stalled{};
    std::uint32_t x{};
    std::uint32_t left{};
};

std::size_t bucket_of(const Tables& t, std::uint8_t model, std::uint32_t slot) noexcept
{
    const auto& start = t.start[model];
    std::size_t b = t.bucket[model][slot >> 8];
    while (start[b + 1] <= slot) {
        ++b;
    }
    return b;
}

void step(Lane& lane, const Tables& t) noexcept
{
    if (lane.phase != 3) {
        if (lane.latch < 0) {
            return;
        }
        const auto b = static_cast<std::uint8_t>(lane.latch);
        lane.latch = -1;
        lane.need = true;
        switch (lane.phase) {
        case 0: {
            const auto group = static_cast<std::uint8_t>(lane.width - (b >> 6));
            lane.model = static_cast<std::uint8_t>(group * 16 + (b & 15));
            lane.reset = (b & 0x10) != 0;
            lane.long_length = (b & 0x20) != 0;
            lane.left = 0;
            lane.phase = 1;
            break;
        }
        case 1:
            if (lane.long_length) {
                lane.left = b;
                lane.phase = 2;
            } else {
                lane.left = static_cast<std::uint32_t>(b) + 1;
                lane.phase = 3;
            }
            break;
        default:
            lane.left = ((static_cast<std::uint32_t>(b) << 8) | lane.left) + 257;
            lane.phase = 3;
            break;
        }
        return;
    }

    std::uint32_t x = lane.x;
    if (x < kLow && lane.latch >= 0) {
        const auto b = static_cast<std::uint8_t>(lane.latch);
        lane.latch = -1;
        lane.left -= 1;
        x = (x << 8) | b;
    }
    lane.need = lane.left != 0 && lane.latch < 0;
    if (x > kLow) {
        const auto& freq = t.freq[lane.model];
        const auto& start = t.start[lane.model];
        const std::uint32_t slot = x & 0xffff;
        const std::size_t b = bucket_of(t, lane.model, slot);
        std::uint32_t symbol = 0;
        std::uint32_t f = 0;
        std::uint32_t rem = 0;
        if (b == 0) {
            symbol = 0;
            f = freq[0] - 1;
            rem = slot - 1;
        } else {
            const std::uint32_t offset = slot - start[b];
            const auto q = static_cast<std::uint32_t>((static_cast<std::uint64_t>(offset)
                                                       * t.recip[lane.model][b])
                                                      >> 32);
            symbol = (1u << (b - 1)) + q;
            f = freq[b];
            rem = offset - q * freq[b];
        }
        lane.x = (x >> 16) * f + rem;
        lane.out = static_cast<std::int16_t>(symbol & 0xff);
    } else if (lane.left != 0) {
        lane.x = x;
    } else {
        lane.phase = 0;
        lane.need = lane.latch < 0;
        if (lane.reset) {
            lane.x = 0;
        }
    }
}

template <typename Fetch>
std::expected<void, Error> run_lanes(const Tables& t, std::span<const std::uint8_t> widths,
                                     std::size_t per, std::size_t blocks, Fetch&& fetch,
                                     std::span<std::uint8_t> records)
{
    std::array<Lane, 16> lanes{};
    const std::size_t count = widths.size();
    for (std::size_t j = 0; j < count; ++j) {
        lanes[j].width = widths[j];
        lanes[j].need = true;
    }
    const std::size_t stride = count * per;
    const std::uint32_t full = (1u << count) - 1;
    std::size_t position = 0;
    std::uint32_t done = 0;
    int idle = 0;
    std::size_t at = 0;
    while (position < blocks * per) {
        int taken = 0;
        bool moved = false;
        for (std::size_t j = 0; j < count; ++j) {
            Lane& lane = lanes[j];
            const std::uint32_t bit = 1u << j;
            const int before = taken;
            if (!lane.stalled) {
                step(lane, t);
            }
            const bool waiting = lane.out >= 0 && (done & bit) != 0;
            if (lane.need && !waiting && taken < kRoundBytes) {
                const int value = fetch(j);
                if (value >= 0) {
                    lane.latch = static_cast<std::int16_t>(value);
                    ++taken;
                }
            }
            lane.stalled = (lane.need && before >= kRoundBytes) || waiting;
            if (!lane.stalled && lane.out >= 0) {
                records[at + j * per] = static_cast<std::uint8_t>(lane.out);
                lane.out = -1;
                done |= bit;
                moved = true;
            }
        }
        if (done == full) {
            ++position;
            at += position % per == 0 ? stride - per + 1 : 1;
            done = 0;
            moved = true;
        }
        idle = taken > 0 || moved ? 0 : idle + 1;
        if (idle > 3) {
            return std::unexpected(Error{"packed stream is truncated or corrupt"});
        }
    }
    return {};
}

std::uint8_t interleave(std::uint8_t a, std::uint8_t b) noexcept
{
    const auto spread = [](std::uint8_t v) {
        std::uint8_t result = 0;
        for (int k = 0; k < 4; ++k) {
            result = static_cast<std::uint8_t>(result | (((v >> k) & 1) << (2 * k)));
        }
        return result;
    };
    return static_cast<std::uint8_t>(spread(a) | (spread(b) << 1));
}

std::pair<std::size_t, std::size_t> references(const std::uint8_t* s, bool paired) noexcept
{
    std::uint8_t d = s[0];
    std::uint8_t ix = s[1];
    if (paired) {
        d = interleave(s[0], s[1]);
        ix = interleave(s[2], s[3]);
    }
    const auto delta = static_cast<std::uint8_t>((d >> 1) ^ static_cast<std::uint8_t>(0u - (d & 1u)));
    return {static_cast<std::uint8_t>(ix + delta), ix};
}

template <typename T>
const T& pick(std::size_t i, std::size_t r, const std::vector<T>& history,
              const std::vector<T>& dictionary, const T& zero) noexcept
{
    if (r == 0) {
        return zero;
    }
    if (r <= i) {
        return history[(i - r) % kHistory];
    }
    return dictionary[r - i - 1];
}

std::uint8_t zz(std::uint8_t v, std::uint32_t bits) noexcept
{
    const auto mask = static_cast<std::uint8_t>((1u << bits) - 1);
    return static_cast<std::uint8_t>(((v >> 1) ^ ((v & 1) != 0 ? mask : 0)) & mask);
}

struct Widths {
    std::array<std::uint8_t, 8> ep{};
    std::array<std::uint8_t, 32> ix{};
    bool paired{};
};

constexpr Widths make_widths(std::array<std::uint8_t, 8> ep, std::size_t ix3, std::size_t ix2,
                             bool paired)
{
    Widths result{ep, {}, paired};
    for (std::size_t k = 0; k < ix3 + ix2; ++k) {
        result.ix[k] = k < ix3 ? 3 : 2;
    }
    return result;
}

constexpr Widths kBc1Widths = make_widths({5, 6, 5, 5, 6, 5, 0, 0}, 0, 16, false);
constexpr Widths kBc3Widths = make_widths({5, 6, 5, 5, 6, 5, 8, 8}, 16, 16, true);
constexpr Widths kBc4Widths = make_widths({8, 8, 0, 0, 0, 0, 0, 0}, 16, 0, false);
constexpr Widths kBc5Widths = make_widths({8, 8, 8, 8, 0, 0, 0, 0}, 32, 0, true);

void quads(const std::array<std::uint8_t, 256>& rank, const std::uint8_t* s, std::size_t count,
           std::uint8_t* ix) noexcept
{
    constexpr std::array<std::size_t, 4> kOffsets{0, 1, 4, 5};
    for (std::size_t q = 0; q < count; ++q) {
        const std::uint8_t v = rank[s[q]];
        const std::size_t base = (q >> 1) * 8 + (q & 1) * 2;
        for (std::size_t k = 0; k < 4; ++k) {
            ix[base + kOffsets[k]] = static_cast<std::uint8_t>((v >> (2 * k)) & 3);
        }
    }
}

void pairs(const std::array<std::uint8_t, 64>& rank, const std::uint8_t* s, std::size_t count,
           std::uint8_t* ix) noexcept
{
    for (std::size_t k = 0; k < count; ++k) {
        const std::uint8_t v = rank[s[k] & 63];
        ix[2 * k] = static_cast<std::uint8_t>(v & 7);
        ix[2 * k + 1] = static_cast<std::uint8_t>((v >> 4) & 7);
    }
}

Plain bc1_residual(const Tables& t, const std::uint8_t* s) noexcept
{
    constexpr std::array<std::uint32_t, 6> kBits{5, 6, 5, 5, 6, 5};
    Plain v{};
    for (std::size_t k = 0; k < kBits.size(); ++k) {
        v.ep[k] = zz(s[2 + k], kBits[k]);
    }
    quads(t.rank1, s + 8, 4, v.ix.data());
    return v;
}

Plain bc3_residual(const Tables& t, const std::uint8_t* s) noexcept
{
    Plain v{};
    v.ep = {zz(s[14], 5), zz(s[16], 6), zz(s[18], 5), zz(s[15], 5),
            zz(s[17], 6), zz(s[19], 5), zz(s[4], 8),  zz(s[5], 8)};
    pairs(t.rank3a, s + 6, 8, v.ix.data());
    quads(t.rank3c, s + 20, 4, v.ix.data() + 16);
    return v;
}

Plain bc4_residual(const Tables& t, const std::uint8_t* s) noexcept
{
    Plain v{};
    v.ep[0] = zz(s[2], 8);
    v.ep[1] = zz(s[3], 8);
    pairs(t.rank4, s + 4, 8, v.ix.data());
    return v;
}

Plain bc5_residual(const Tables& t, const std::uint8_t* s) noexcept
{
    Plain v{};
    for (std::size_t k = 0; k < 4; ++k) {
        v.ep[k] = zz(s[4 + k], 8);
    }
    pairs(t.rank5, s + 8, 16, v.ix.data());
    return v;
}

constexpr std::array<std::uint32_t, 4> kBc1Code{0, 2, 3, 1};
constexpr std::array<std::uint64_t, 8> kBc4Code{0, 2, 3, 4, 5, 6, 7, 1};

void store64(std::uint8_t* out, std::uint64_t value) noexcept
{
    for (int k = 0; k < 8; ++k) {
        out[k] = static_cast<std::uint8_t>(value >> (8 * k));
    }
}

void bc1_color(const std::uint8_t* ep, const std::uint8_t* ix, std::uint8_t* out) noexcept
{
    const auto c0 = static_cast<std::uint16_t>((ep[0] << 11) | (ep[1] << 5) | ep[2]);
    const auto c1 = static_cast<std::uint16_t>((ep[3] << 11) | (ep[4] << 5) | ep[5]);
    std::uint32_t bits = 0;
    for (std::uint32_t p = 0; p < 16; ++p) {
        bits |= kBc1Code[ix[p] & 3] << (2 * p);
    }
    out[0] = static_cast<std::uint8_t>(c0);
    out[1] = static_cast<std::uint8_t>(c0 >> 8);
    out[2] = static_cast<std::uint8_t>(c1);
    out[3] = static_cast<std::uint8_t>(c1 >> 8);
    for (int k = 0; k < 4; ++k) {
        out[4 + k] = static_cast<std::uint8_t>(bits >> (8 * k));
    }
}

void bc4_alpha(std::uint8_t e0, std::uint8_t e1, const std::uint8_t* ix, std::uint8_t* out) noexcept
{
    std::uint64_t bits = 0;
    for (std::uint32_t p = 0; p < 16; ++p) {
        bits |= kBc4Code[ix[p] & 7] << (16 + 3 * p);
    }
    const std::uint64_t value = static_cast<std::uint64_t>(e0 ^ 0x80)
                                | (static_cast<std::uint64_t>(e1 ^ 0x80) << 8) | bits;
    store64(out, value);
}

void bc1_pack(const Plain& v, std::uint8_t* out) noexcept
{
    bc1_color(v.ep.data(), v.ix.data(), out);
}

void bc3_pack(const Plain& v, std::uint8_t* out) noexcept
{
    bc4_alpha(v.ep[6], v.ep[7], v.ix.data(), out);
    bc1_color(v.ep.data(), v.ix.data() + 16, out + 8);
}

void bc4_pack(const Plain& v, std::uint8_t* out) noexcept
{
    bc4_alpha(v.ep[0], v.ep[1], v.ix.data(), out);
}

void bc5_pack(const Plain& v, std::uint8_t* out) noexcept
{
    bc4_alpha(v.ep[0], v.ep[1], v.ix.data(), out);
    bc4_alpha(v.ep[2], v.ep[3], v.ix.data() + 16, out + 8);
}

using Residual = Plain (*)(const Tables&, const std::uint8_t*) noexcept;
using Pack = void (*)(const Plain&, std::uint8_t*) noexcept;

void decode_plain(const Tables& t, std::span<const std::uint8_t> records, std::size_t stride,
                  std::span<std::uint8_t> out, std::size_t block, const std::vector<Plain>& dictionary,
                  const Widths& w, Residual residual, Pack pack)
{
    std::vector<Plain> history(kHistory);
    const Plain zero{};
    const std::size_t blocks = out.size() / block;
    for (std::size_t i = 0; i < blocks; ++i) {
        const std::uint8_t* s = records.data() + i * stride;
        const auto [ep_ref, ix_ref] = references(s, w.paired);
        const auto& ep_base = pick(i, ep_ref, history, dictionary, zero).ep;
        const auto& ix_base = pick(i, ix_ref, history, dictionary, zero).ix;
        Plain v = residual(t, s);
        for (std::size_t k = 0; k < 8; ++k) {
            const auto mask = static_cast<std::uint8_t>((1u << w.ep[k]) - 1);
            v.ep[k] = static_cast<std::uint8_t>((v.ep[k] + ep_base[k]) & mask);
        }
        for (std::size_t k = 0; k < 32; ++k) {
            const auto mask = static_cast<std::uint8_t>((1u << w.ix[k]) - 1);
            v.ix[k] = static_cast<std::uint8_t>((v.ix[k] + ix_base[k]) & mask);
        }
        history[i % kHistory] = v;
        pack(v, out.data() + i * block);
    }
}

std::array<std::int16_t, 2> nibbles(std::uint8_t b, bool odd_first) noexcept
{
    const auto half = [](std::uint32_t v) {
        std::uint32_t result = 0;
        for (std::uint32_t k = 0; k < 4; ++k) {
            result |= ((v >> (2 * k)) & 1) << k;
        }
        return result;
    };
    const auto z = [](std::uint32_t u) {
        return static_cast<std::int16_t>(static_cast<std::int16_t>(u >> 1)
                                         ^ -static_cast<std::int16_t>(u & 1));
    };
    const std::int16_t even = z(half(b));
    const std::int16_t odd = z(half(static_cast<std::uint32_t>(b) >> 1));
    if (odd_first) {
        return {odd, even};
    }
    return {even, odd};
}

std::int16_t add_index(std::int16_t current, std::uint8_t bits, std::int16_t reference,
                       std::uint8_t reference_bits) noexcept
{
    const auto up = [](std::int16_t v, std::uint8_t b) -> std::uint16_t {
        if (b == 0 || b > 16) {
            return 0;
        }
        return static_cast<std::uint16_t>(static_cast<std::uint16_t>(v) << (16 - b));
    };
    const auto sum = static_cast<std::int16_t>(
        static_cast<std::uint16_t>(up(current, bits) + up(reference, reference_bits)));
    if (bits == 0) {
        return 0;
    }
    return static_cast<std::int16_t>(sum >> (16 - bits));
}

class Bits {
public:
    void put(std::uint64_t value, std::uint32_t n) noexcept
    {
        if (n > 0 && at_ < 128) {
            const std::uint64_t mask = n >= 64 ? ~0ull : (1ull << n) - 1;
            const std::uint64_t masked = value & mask;
            if (at_ < 64) {
                low_ |= masked << at_;
                if (at_ > 0 && at_ + n > 64) {
                    high_ |= masked >> (64 - at_);
                }
            } else {
                high_ |= masked << (at_ - 64);
            }
        }
        at_ += n;
    }

    void store(std::uint8_t* out) const noexcept
    {
        store64(out, low_);
        store64(out + 8, high_);
    }

    std::array<std::uint8_t, 16> bytes() const noexcept
    {
        std::array<std::uint8_t, 16> out{};
        store(out.data());
        return out;
    }

private:
    std::uint64_t low_{};
    std::uint64_t high_{};
    std::uint32_t at_{};
};

constexpr std::array<std::uint8_t, 64> kAnchor2{
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2,  8,  2,  2, 8,
    8,  15, 2,  8,  2,  2,  8,  8,  2,  2,  15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2, 2,
    2,  15, 15, 6,  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15,
};
constexpr std::array<std::uint8_t, 64> kAnchor3a{
    3,  3, 15, 15, 8, 3,  15, 15, 8,  8,  6,  6, 6,  5, 3,  3,  3,  3,  8, 15, 3, 3,
    6,  10, 5, 8,  8, 6,  8,  5,  15, 15, 8,  15, 3, 5, 6,  10, 8,  15, 15, 3, 15, 5,
    15, 15, 15, 15, 3, 15, 5,  5,  5,  8,  5, 10, 5, 10, 8,  13, 15, 12, 3, 3,
};
constexpr std::array<std::uint8_t, 64> kAnchor3b{
    15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,  15, 8,  15, 3,  15, 8,
    15, 8,  3,  15, 6,  10, 15, 15, 10, 8,  15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15,
    3,  6,  6,  8,  15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8,
};

constexpr std::array<std::array<std::uint8_t, 4>, 8> kBc7Bits{{
    {4, 4, 4, 0},
    {6, 6, 6, 0},
    {5, 5, 5, 0},
    {7, 7, 7, 0},
    {5, 5, 5, 6},
    {7, 7, 7, 8},
    {7, 7, 7, 7},
    {5, 5, 5, 5},
}};

constexpr std::array<std::array<std::uint8_t, 2>, 8> kBc7Index{{
    {3, 0},
    {3, 0},
    {2, 0},
    {2, 0},
    {3, 2},
    {2, 2},
    {4, 0},
    {2, 0},
}};

bool has_pbits(std::uint32_t mode) noexcept
{
    return mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7;
}

std::array<std::uint8_t, 4> bc7_precision(std::uint16_t ctrl) noexcept
{
    const std::uint32_t mode = ctrl & 7u;
    const std::uint32_t rotation = (ctrl >> 3) & 3u;
    std::array<std::uint8_t, 4> p = kBc7Bits[mode];
    if (rotation != 0) {
        std::swap(p[3], p[rotation - 1]);
    } else if (has_pbits(mode)) {
        p[0] = static_cast<std::uint8_t>(p[0] + 1);
    }
    return p;
}

void bc7_pack(const Bc7Ep& ep, const Bc7Ix& ix, const std::array<std::uint8_t, 4>& prec,
              std::uint8_t* out) noexcept
{
    const std::uint16_t ctrl = ep.ctrl;
    const std::uint32_t mode = ctrl & 7u;
    const std::size_t subsets = (ctrl & 0xffu) >> 6;
    const std::size_t part = (ctrl >> 8) & 63u;
    const std::uint32_t rotation = (ctrl >> 3) & 3u;
    Bits w;
    w.put(1ull << mode, mode + 1);
    switch (mode) {
    case 0:
        w.put(part, 4);
        break;
    case 1:
    case 2:
    case 3:
    case 7:
        w.put(part, 6);
        break;
    case 4:
        w.put(rotation, 2);
        w.put((ctrl >> 5) & 1u, 1);
        break;
    case 5:
        w.put(rotation, 2);
        break;
    default:
        break;
    }
    const std::size_t ends = 2 * subsets;
    const auto field = [&](std::size_t row, std::size_t col, std::uint8_t p) -> std::uint64_t {
        return static_cast<std::uint64_t>(ep.rows[row][col] >> (16 - p));
    };
    std::array<std::size_t, 4> order{0, 1, 2, 3};
    if (rotation != 0) {
        std::swap(order[3], order[rotation - 1]);
    }
    for (const std::size_t channel : order) {
        const std::uint8_t p = prec[channel];
        if (p == 0) {
            continue;
        }
        const std::uint32_t drop = channel == 0 && has_pbits(mode) ? 1u : 0u;
        for (std::size_t e = 0; e < ends; ++e) {
            w.put(field(e / 2, 2 * channel + e % 2, p) >> drop, p - drop);
        }
    }
    if (mode == 1) {
        for (std::size_t row = 0; row < subsets; ++row) {
            w.put(field(row, 0, prec[0]) & 1, 1);
        }
    } else if (has_pbits(mode)) {
        for (std::size_t e = 0; e < ends; ++e) {
            w.put(field(e / 2, e % 2, prec[0]) & 1, 1);
        }
    }
    std::array<std::uint8_t, 3> anchors{0, 0xff, 0xff};
    if (subsets == 2) {
        anchors[1] = kAnchor2[part];
    } else if (subsets == 3) {
        anchors[1] = kAnchor3a[part];
        anchors[2] = kAnchor3b[part];
    }
    const auto indices = [&](std::size_t set) {
        const std::uint32_t bits = ix.bits[set];
        for (std::size_t p = 0; p < 16; ++p) {
            const bool anchor = std::find(anchors.begin(), anchors.end(), static_cast<std::uint8_t>(p))
                                != anchors.end();
            const std::uint32_t n = bits - (anchor ? 1u : 0u);
            if (bits == 0) {
                continue;
            }
            w.put(static_cast<std::uint16_t>(ix.ix[set][p]), n);
        }
    };
    if (mode == 4) {
        indices(1);
        indices(0);
    } else if (mode == 5) {
        indices(0);
        indices(1);
    } else {
        indices(0);
    }
    w.store(out);
}

std::expected<void, Error> decode_bc7(const Tables& t, std::span<const std::uint8_t> records,
                                      std::size_t stride, std::span<std::uint8_t> out)
{
    std::vector<Bc7Entry> history(kHistory);
    const Bc7Entry zero{};
    const std::size_t blocks = out.size() / 16;
    constexpr std::array<std::array<std::size_t, 4>, 2> kOffsets{{{22, 24, 26, 28}, {12, 14, 16, 20}}};
    for (std::size_t i = 0; i < blocks; ++i) {
        const std::uint8_t* s = records.data() + i * stride;
        const auto [ep_ref, ix_ref] = references(s, true);
        const std::uint16_t ctrl = t.ctrl7[((s[31] & 15u) << 5) | (s[30] & 31u)];
        if (ctrl == 0) {
            return std::unexpected(Error{"BC7 control code is not defined"});
        }
        const std::uint32_t mode = ctrl & 7u;
        const std::size_t subsets = (ctrl & 0xffu) >> 6;
        const auto prec = bc7_precision(ctrl);
        const bool three = subsets == 3;
        const auto or0 = [&](std::uint8_t v) -> std::uint8_t { return three ? 0 : v; };
        const auto if3 = [&](std::uint8_t v) -> std::uint8_t { return three ? v : 0; };
        const std::array<std::array<std::uint8_t, 8>, 3> stored{{
            {s[4], s[5], s[6], s[7], s[8], s[9], or0(s[10]), or0(s[11])},
            {s[12], s[13], s[14], s[15], s[16], s[17], or0(s[18]), or0(s[19])},
            {s[20], s[21], if3(s[10]), if3(s[11]), if3(s[18]), if3(s[19]), 0, 0},
        }};
        const Bc7Ep& r = pick(i, ep_ref, history, t.bc7, zero).ep;
        Bc7Ep ep{};
        ep.ctrl = ctrl;
        const std::size_t common = std::min<std::size_t>(subsets, (r.ctrl & 0xffu) >> 6);
        const std::size_t columns = prec[3] > 0 ? 8 : 6;
        for (std::size_t c = 0; c < columns; ++c) {
            const std::uint8_t p = prec[c / 2];
            if (p == 0) {
                continue;
            }
            const auto mask = static_cast<std::uint16_t>(0xffffu << (16 - p));
            for (std::size_t row = 0; row < std::min<std::size_t>(subsets, 3); ++row) {
                const std::uint8_t v = stored[row][c];
                const std::uint16_t flip = (v & 1) != 0 ? 0xffff : 0;
                const auto residual = static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>((v >> 1) ^ flip) << (16 - p));
                const auto base = static_cast<std::uint16_t>(r.rows[row < common ? row : 0][c] & mask);
                ep.rows[row][c] = static_cast<std::uint16_t>(base + residual);
            }
        }
        const auto bits = kBc7Index[mode];
        const Bc7Ix& rx = pick(i, ix_ref, history, t.bc7, zero).ix;
        Bc7Ix ix{};
        ix.bits = bits;
        for (std::size_t set = 0; set < 2; ++set) {
            if (bits[set] == 0) {
                continue;
            }
            for (std::size_t k = 0; k < 4; ++k) {
                const std::size_t o = kOffsets[set][k];
                for (std::size_t b = 0; b < 2; ++b) {
                    const auto pair = nibbles(s[o + b], k < 3);
                    for (std::size_t h = 0; h < 2; ++h) {
                        const std::size_t p = 4 * k + 2 * b + h;
                        ix.ix[set][p] = add_index(pair[h], bits[set], rx.ix[set][p], rx.bits[set]);
                    }
                }
            }
        }
        history[i % kHistory] = {ep, ix};
        bc7_pack(ep, ix, prec, out.data() + i * 16);
    }
    return {};
}

constexpr std::array<std::array<std::uint8_t, 4>, 15> kBc6Bits{{
    {0, 0, 0, 0},
    {10, 5, 5, 5},
    {7, 6, 6, 6},
    {11, 5, 4, 4},
    {11, 4, 5, 4},
    {11, 4, 4, 5},
    {9, 5, 5, 5},
    {8, 6, 5, 5},
    {8, 5, 6, 5},
    {8, 5, 5, 6},
    {6, 6, 6, 6},
    {10, 10, 10, 10},
    {11, 9, 9, 9},
    {12, 8, 8, 8},
    {16, 4, 4, 4},
}};

constexpr std::array<std::pair<std::uint64_t, std::uint32_t>, 15> kBc6Code{{
    {0, 0},
    {0, 2},
    {1, 2},
    {2, 5},
    {6, 5},
    {10, 5},
    {14, 5},
    {18, 5},
    {22, 5},
    {26, 5},
    {30, 5},
    {3, 5},
    {7, 5},
    {11, 5},
    {15, 5},
}};

struct Run {
    std::uint8_t channel;
    std::uint8_t endpoint;
    std::uint8_t first;
    std::uint8_t length;
};

constexpr std::uint8_t kR = 0;
constexpr std::uint8_t kG = 1;
constexpr std::uint8_t kB = 2;
constexpr Run kPartition{3, 0, 0, 5};

constexpr Run run(std::uint8_t channel, std::uint8_t endpoint, std::uint8_t first,
                  std::uint8_t length)
{
    return {channel, endpoint, first, length};
}

constexpr Run kMode1[] = {
    run(kG, 2, 4, 1), run(kB, 2, 4, 1), run(kB, 3, 4, 1), run(kR, 0, 0, 10), run(kG, 0, 0, 10),
    run(kB, 0, 0, 10), run(kR, 1, 0, 5), run(kG, 3, 4, 1), run(kG, 2, 0, 4), run(kG, 1, 0, 5),
    run(kB, 3, 0, 1), run(kG, 3, 0, 4), run(kB, 1, 0, 5), run(kB, 3, 1, 1), run(kB, 2, 0, 4),
    run(kR, 2, 0, 5), run(kB, 3, 2, 1), run(kR, 3, 0, 5), run(kB, 3, 3, 1), kPartition,
};
constexpr Run kMode2[] = {
    run(kG, 2, 5, 1), run(kG, 3, 4, 1), run(kG, 3, 5, 1), run(kR, 0, 0, 7), run(kB, 3, 0, 1),
    run(kB, 3, 1, 1), run(kB, 2, 4, 1), run(kG, 0, 0, 7), run(kB, 2, 5, 1), run(kB, 3, 2, 1),
    run(kG, 2, 4, 1), run(kB, 0, 0, 7), run(kB, 3, 3, 1), run(kB, 3, 5, 1), run(kB, 3, 4, 1),
    run(kR, 1, 0, 6), run(kG, 2, 0, 4), run(kG, 1, 0, 6), run(kG, 3, 0, 4), run(kB, 1, 0, 6),
    run(kB, 2, 0, 4), run(kR, 2, 0, 6), run(kR, 3, 0, 6), kPartition,
};
constexpr Run kMode3[] = {
    run(kR, 0, 0, 10), run(kG, 0, 0, 10), run(kB, 0, 0, 10), run(kR, 1, 0, 5), run(kR, 0, 10, 1),
    run(kG, 2, 0, 4),  run(kG, 1, 0, 4),  run(kG, 0, 10, 1), run(kB, 3, 0, 1), run(kG, 3, 0, 4),
    run(kB, 1, 0, 4),  run(kB, 0, 10, 1), run(kB, 3, 1, 1),  run(kB, 2, 0, 4), run(kR, 2, 0, 5),
    run(kB, 3, 2, 1),  run(kR, 3, 0, 5),  run(kB, 3, 3, 1),  kPartition,
};
constexpr Run kMode4[] = {
    run(kR, 0, 0, 10), run(kG, 0, 0, 10), run(kB, 0, 0, 10), run(kR, 1, 0, 4), run(kR, 0, 10, 1),
    run(kG, 3, 4, 1),  run(kG, 2, 0, 4),  run(kG, 1, 0, 5),  run(kG, 0, 10, 1), run(kG, 3, 0, 4),
    run(kB, 1, 0, 4),  run(kB, 0, 10, 1), run(kB, 3, 1, 1),  run(kB, 2, 0, 4), run(kR, 2, 0, 4),
    run(kB, 3, 0, 1),  run(kB, 3, 2, 1),  run(kR, 3, 0, 4),  run(kG, 2, 4, 1), run(kB, 3, 3, 1),
    kPartition,
};
constexpr Run kMode5[] = {
    run(kR, 0, 0, 10), run(kG, 0, 0, 10), run(kB, 0, 0, 10), run(kR, 1, 0, 4), run(kR, 0, 10, 1),
    run(kB, 2, 4, 1),  run(kG, 2, 0, 4),  run(kG, 1, 0, 4),  run(kG, 0, 10, 1), run(kB, 3, 0, 1),
    run(kG, 3, 0, 4),  run(kB, 1, 0, 5),  run(kB, 0, 10, 1), run(kB, 2, 0, 4), run(kR, 2, 0, 4),
    run(kB, 3, 1, 1),  run(kB, 3, 2, 1),  run(kR, 3, 0, 4),  run(kB, 3, 4, 1), run(kB, 3, 3, 1),
    kPartition,
};
constexpr Run kMode6[] = {
    run(kR, 0, 0, 9), run(kB, 2, 4, 1), run(kG, 0, 0, 9), run(kG, 2, 4, 1), run(kB, 0, 0, 9),
    run(kB, 3, 4, 1), run(kR, 1, 0, 5), run(kG, 3, 4, 1), run(kG, 2, 0, 4), run(kG, 1, 0, 5),
    run(kB, 3, 0, 1), run(kG, 3, 0, 4), run(kB, 1, 0, 5), run(kB, 3, 1, 1), run(kB, 2, 0, 4),
    run(kR, 2, 0, 5), run(kB, 3, 2, 1), run(kR, 3, 0, 5), run(kB, 3, 3, 1), kPartition,
};
constexpr Run kMode7[] = {
    run(kR, 0, 0, 8), run(kG, 3, 4, 1), run(kB, 2, 4, 1), run(kG, 0, 0, 8), run(kB, 3, 2, 1),
    run(kG, 2, 4, 1), run(kB, 0, 0, 8), run(kB, 3, 3, 1), run(kB, 3, 4, 1), run(kR, 1, 0, 6),
    run(kG, 2, 0, 4), run(kG, 1, 0, 5), run(kB, 3, 0, 1), run(kG, 3, 0, 4), run(kB, 1, 0, 5),
    run(kB, 3, 1, 1), run(kB, 2, 0, 4), run(kR, 2, 0, 6), run(kR, 3, 0, 6), kPartition,
};
constexpr Run kMode8[] = {
    run(kR, 0, 0, 8), run(kB, 3, 0, 1), run(kB, 2, 4, 1), run(kG, 0, 0, 8), run(kG, 2, 5, 1),
    run(kG, 2, 4, 1), run(kB, 0, 0, 8), run(kG, 3, 5, 1), run(kB, 3, 4, 1), run(kR, 1, 0, 5),
    run(kG, 3, 4, 1), run(kG, 2, 0, 4), run(kG, 1, 0, 6), run(kG, 3, 0, 4), run(kB, 1, 0, 5),
    run(kB, 3, 1, 1), run(kB, 2, 0, 4), run(kR, 2, 0, 5), run(kB, 3, 2, 1), run(kR, 3, 0, 5),
    run(kB, 3, 3, 1), kPartition,
};
constexpr Run kMode9[] = {
    run(kR, 0, 0, 8), run(kB, 3, 1, 1), run(kB, 2, 4, 1), run(kG, 0, 0, 8), run(kB, 2, 5, 1),
    run(kG, 2, 4, 1), run(kB, 0, 0, 8), run(kB, 3, 5, 1), run(kB, 3, 4, 1), run(kR, 1, 0, 5),
    run(kG, 3, 4, 1), run(kG, 2, 0, 4), run(kG, 1, 0, 5), run(kB, 3, 0, 1), run(kG, 3, 0, 4),
    run(kB, 1, 0, 6), run(kB, 2, 0, 4), run(kR, 2, 0, 5), run(kB, 3, 2, 1), run(kR, 3, 0, 5),
    run(kB, 3, 3, 1), kPartition,
};
constexpr Run kMode10[] = {
    run(kR, 0, 0, 6), run(kG, 3, 4, 1), run(kB, 3, 0, 1), run(kB, 3, 1, 1), run(kB, 2, 4, 1),
    run(kG, 0, 0, 6), run(kG, 2, 5, 1), run(kB, 2, 5, 1), run(kB, 3, 2, 1), run(kG, 2, 4, 1),
    run(kB, 0, 0, 6), run(kG, 3, 5, 1), run(kB, 3, 3, 1), run(kB, 3, 5, 1), run(kB, 3, 4, 1),
    run(kR, 1, 0, 6), run(kG, 2, 0, 4), run(kG, 1, 0, 6), run(kG, 3, 0, 4), run(kB, 1, 0, 6),
    run(kB, 2, 0, 4), run(kR, 2, 0, 6), run(kR, 3, 0, 6), kPartition,
};
constexpr Run kMode11[] = {
    run(kR, 0, 0, 10), run(kG, 0, 0, 10), run(kB, 0, 0, 10),
    run(kR, 1, 0, 10), run(kG, 1, 0, 10), run(kB, 1, 0, 10),
};
constexpr Run kMode12[] = {
    run(kR, 0, 0, 10), run(kG, 0, 0, 10), run(kB, 0, 0, 10), run(kR, 1, 0, 9), run(kR, 0, 10, 1),
    run(kG, 1, 0, 9),  run(kG, 0, 10, 1), run(kB, 1, 0, 9),  run(kB, 0, 10, 1),
};
constexpr Run kMode13[] = {
    run(kR, 0, 0, 10), run(kG, 0, 0, 10), run(kB, 0, 0, 10), run(kR, 1, 0, 8), run(kR, 0, 10, 2),
    run(kG, 1, 0, 8),  run(kG, 0, 10, 2), run(kB, 1, 0, 8),  run(kB, 0, 10, 2),
};
constexpr Run kMode14[] = {
    run(kR, 0, 0, 10), run(kG, 0, 0, 10), run(kB, 0, 0, 10), run(kR, 1, 0, 4), run(kR, 0, 10, 6),
    run(kG, 1, 0, 4),  run(kG, 0, 10, 6), run(kB, 1, 0, 4),  run(kB, 0, 10, 6),
};

constexpr std::array<std::span<const Run>, 15> kBc6Layout{
    std::span<const Run>{}, kMode1,  kMode2,  kMode3,  kMode4,  kMode5,  kMode6,  kMode7,
    kMode8,                 kMode9,  kMode10, kMode11, kMode12, kMode13, kMode14,
};

std::uint8_t bc6_index_bits(std::uint8_t mode) noexcept
{
    if (mode >= 1 && mode <= 10) {
        return 3;
    }
    if (mode >= 11 && mode <= 14) {
        return 4;
    }
    return 0;
}

std::expected<void, Error> decode_bc6(const Tables& t, std::span<const std::uint8_t> records,
                                      std::size_t stride, std::span<std::uint8_t> out)
{
    std::vector<Bc6Entry> history(kHistory);
    Bc6Entry zero{};
    zero.ix.mode = 11;
    const auto z = [](std::uint16_t v) {
        return static_cast<std::uint16_t>((v >> 1) ^ static_cast<std::uint16_t>(0u - (v & 1u)));
    };
    const auto wide = [](std::uint8_t a, std::uint8_t b) {
        std::uint32_t result = 0;
        for (std::uint32_t k = 0; k < 8; ++k) {
            result |= ((static_cast<std::uint32_t>(a) >> k) & 1u) << (2 * k);
            result |= ((static_cast<std::uint32_t>(b) >> k) & 1u) << (2 * k + 1);
        }
        return static_cast<std::uint16_t>(result);
    };
    const auto mask = [](std::uint32_t shift) { return static_cast<std::uint16_t>(0xffffu << shift); };
    const std::size_t blocks = out.size() / 16;
    for (std::size_t i = 0; i < blocks; ++i) {
        const std::uint8_t* s = records.data() + i * stride;
        const auto [ep_ref, ix_ref] = references(s, true);
        const auto mode = static_cast<std::uint8_t>(s[30] + 1);
        if (mode < 1 || mode > 14) {
            return std::unexpected(Error{"BC6H mode is out of range"});
        }
        const bool two = mode < 11;
        std::array<std::uint32_t, 4> shift{};
        for (std::size_t k = 0; k < 4; ++k) {
            shift[k] = 16u - kBc6Bits[mode][k];
        }
        const std::array<std::uint16_t, 3> e0{z(wide(s[4], s[5])), z(wide(s[6], s[7])),
                                              z(wide(s[8], s[9]))};
        const std::array<std::uint16_t, 3> e1{z(wide(s[10], s[11])), z(wide(s[12], s[13])),
                                              z(wide(s[14], s[15]))};
        std::array<std::uint16_t, 3> e2 = e0;
        std::array<std::uint16_t, 3> e3 = e1;
        if (two) {
            e2 = {z(s[16]), z(s[18]), z(s[20])};
            e3 = {z(s[17]), z(s[19]), z(s[21])};
        }
        const Bc6Ep& r = pick(i, ep_ref, history, t.bc6, zero).ep;
        Bc6Ep ep{};
        for (std::size_t c = 0; c < 3; ++c) {
            const auto residual = static_cast<std::uint16_t>(e0[c] << shift[0]);
            ep.e[0][c] = static_cast<std::uint16_t>(static_cast<std::uint16_t>(r.e[0][c] + residual)
                                                    & mask(shift[0]));
        }
        const std::array<const std::array<std::uint16_t, 3>*, 3> rest{&e1, &e2, &e3};
        for (std::size_t k = 0; k < (two ? 3u : 1u); ++k) {
            for (std::size_t c = 0; c < 3; ++c) {
                const auto residual = static_cast<std::uint16_t>((*rest[k])[c] << shift[1 + c]);
                ep.e[k + 1][c] = static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(r.e[k + 1][c] + residual) & mask(shift[1 + c]));
            }
        }
        if (!two) {
            for (std::size_t c = 0; c < 3; ++c) {
                ep.e[2][c] = static_cast<std::uint16_t>((ep.e[0][c] >> shift[0]) << shift[1]);
            }
            ep.e[3] = ep.e[1];
        }
        const Bc6Ix& rx = pick(i, ix_ref, history, t.bc6, zero).ix;
        const std::uint8_t bits = bc6_index_bits(mode);
        Bc6Ix ix{};
        ix.mode = mode;
        for (std::size_t k = 0; k < 8; ++k) {
            const auto pair = nibbles(s[22 + k], k < 6);
            for (std::size_t h = 0; h < 2; ++h) {
                const std::size_t p = 2 * k + h;
                ix.ix[p] = add_index(pair[h], bits, rx.ix[p], bc6_index_bits(rx.mode));
            }
        }
        history[i % kHistory] = {ep, ix};
        const auto field = [&](std::size_t k, std::size_t c) -> std::uint64_t {
            const std::uint32_t sh = k == 0 ? shift[0] : shift[1 + c];
            return static_cast<std::uint64_t>(static_cast<std::uint16_t>(ep.e[k][c] ^ 0x8000u) >> sh);
        };
        Bits w;
        w.put(kBc6Code[mode].first, kBc6Code[mode].second);
        const std::uint8_t part = s[31] & 31u;
        for (const Run& item : kBc6Layout[mode]) {
            if (item.channel == 3) {
                w.put(part, 5);
            } else {
                w.put(field(item.endpoint, item.channel) >> item.first, item.length);
            }
        }
        const std::size_t anchor = two ? kAnchor2[part] : 0;
        for (std::size_t p = 0; p < 16; ++p) {
            const bool shortened = p == 0 || p == anchor;
            const auto value = static_cast<std::uint16_t>(static_cast<std::uint16_t>(ix.ix[p])
                                                          + (1u << (bits - 1)));
            w.put(value, bits - (shortened ? 1u : 0u));
        }
        w.store(out.data() + i * 16);
    }
    return {};
}

struct Layout {
    std::span<const std::uint8_t> widths;
    std::size_t per;
};

constexpr std::uint8_t kBc1Lanes[] = {7, 7, 4, 5, 4, 4, 5, 4, 7, 7, 7, 7};
constexpr std::uint8_t kBc3Lanes[] = {3, 3, 7, 5, 5, 5, 5, 4, 5, 4, 7, 7};
constexpr std::uint8_t kBc4Lanes[] = {7, 7, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5};
constexpr std::uint8_t kBc5Lanes[] = {3, 3, 7, 7, 5, 5, 5, 5, 5, 5, 5, 5};
constexpr std::uint8_t kBc6Lanes[] = {3, 3, 7, 7, 7, 4, 4, 4, 5, 5, 5, 7, 7, 7, 7, 4};
constexpr std::uint8_t kBc7Lanes[] = {3, 3, 7, 7, 7, 7, 7, 6, 6, 4, 5, 7, 7, 7, 7, 4};

Layout layout_of(std::uint8_t bc) noexcept
{
    switch (bc) {
    case 1:
        return {kBc1Lanes, 1};
    case 3:
        return {kBc3Lanes, 2};
    case 4:
        return {kBc4Lanes, 1};
    case 5:
        return {kBc5Lanes, 2};
    case 6:
        return {kBc6Lanes, 2};
    default:
        return {kBc7Lanes, 2};
    }
}

}

std::expected<TablesPtr, std::string_view> parse_tables(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() != tables_size) {
        return std::unexpected(std::string_view{"table data has an unexpected size"});
    }
    TablesPtr tables{new Tables{}};
    Tables& t = *tables;
    Reader r{bytes};
    for (std::size_t m = 0; m < kModels; ++m) {
        for (auto& f : t.freq[m]) {
            f = r.u16();
        }
        const std::size_t used = m / 16 + 2;
        std::uint32_t at = 0;
        for (std::size_t b = 0; b < used; ++b) {
            if (t.freq[m][b] == 0) {
                return std::unexpected(std::string_view{"a model bucket has zero frequency"});
            }
            t.start[m][b] = at;
            t.recip[m][b] = ((1ull << 32) + t.freq[m][b] - 1) / t.freq[m][b];
            at += t.freq[m][b] << (b == 0 ? 0 : b - 1);
        }
        if (at != (1u << 16)) {
            return std::unexpected(std::string_view{"a model does not cover 65536 slots"});
        }
        for (std::size_t b = used; b < kBuckets + 1; ++b) {
            t.start[m][b] = 1u << 16;
        }
        for (std::size_t window = 0; window < 256; ++window) {
            const auto first = static_cast<std::uint32_t>(window << 8);
            std::uint8_t count = 0;
            for (std::size_t b = 1; b < kBuckets + 1; ++b) {
                if (t.start[m][b] <= first) {
                    ++count;
                }
            }
            t.bucket[m][window] = count;
        }
    }
    r.fill(t.rank1);
    r.fill(t.rank3a);
    r.fill(t.rank3c);
    r.fill(t.rank4);
    r.fill(t.rank5);
    for (auto& c : t.ctrl7) {
        c = r.u16();
    }
    const auto plain = [&](std::size_t ep, std::size_t ix) {
        std::vector<Plain> entries(kDictionary);
        for (auto& p : entries) {
            for (std::size_t k = 0; k < ep; ++k) {
                p.ep[k] = r.u8();
            }
            for (std::size_t k = 0; k < ix; ++k) {
                p.ix[k] = r.u8();
            }
        }
        return entries;
    };
    t.bc1 = plain(6, 16);
    t.bc3 = plain(8, 32);
    t.bc4 = plain(2, 16);
    t.bc5 = plain(4, 32);
    t.bc6.resize(kDictionary);
    for (auto& entry : t.bc6) {
        r.u8();
        for (auto& endpoint : entry.ep.e) {
            for (auto& v : endpoint) {
                v = r.u16();
            }
        }
        entry.ix.mode = r.u8();
        for (auto& v : entry.ix.ix) {
            v = static_cast<std::int16_t>(r.u16());
        }
    }
    t.bc7.resize(kDictionary);
    for (auto& entry : t.bc7) {
        entry.ep.ctrl = r.u16();
        for (auto& row : entry.ep.rows) {
            for (auto& v : row) {
                v = r.u16();
            }
        }
        entry.ix.bits[0] = r.u8();
        entry.ix.bits[1] = r.u8();
        r.u16();
        for (std::size_t k = 0; k < 31; ++k) {
            entry.ix.ix[k / 16][k % 16] = static_cast<std::int16_t>(r.u16());
        }
    }
    return tables;
}

namespace {

struct Builtin {
    TablesPtr tables;
    std::string_view error;
};

const Builtin& builtin() noexcept
{
    static const Builtin instance = [] {
        Builtin result;
        auto parsed = parse_tables(builtin_table_bytes());
        if (parsed) {
            result.tables = std::move(*parsed);
        } else {
            result.error = parsed.error();
        }
        return result;
    }();
    return instance;
}

}

const Tables* builtin_tables() noexcept
{
    return builtin().tables.get();
}

std::string_view builtin_tables_error() noexcept
{
    return builtin().error;
}

std::expected<void, std::string_view> decode(std::uint8_t bc, std::span<const std::uint8_t> source,
                                             std::span<std::uint8_t> destination,
                                             const Tables& tables)
{
    std::size_t block = 0;
    switch (bc) {
    case 1:
    case 4:
        block = 8;
        break;
    case 3:
    case 5:
    case 6:
    case 7:
        block = 16;
        break;
    default:
        return std::unexpected(std::string_view{"BC format is not supported"});
    }
    if (destination.empty() || destination.size() % block != 0) {
        return std::unexpected(std::string_view{"destination size is not a whole number of blocks"});
    }
    const std::size_t blocks = destination.size() / block;
    const Layout layout = layout_of(bc);
    const std::size_t stride = layout.widths.size() * layout.per;
    std::vector<std::uint8_t> records(blocks * stride);
    std::size_t next = 0;
    const auto fetch = [&](std::size_t) -> int {
        if (next >= source.size()) {
            return -1;
        }
        return source[next++];
    };
    if (auto result = run_lanes(tables, layout.widths, layout.per, blocks, fetch, records); !result) {
        return result;
    }
    const Tables& t = tables;
    switch (bc) {
    case 1:
        decode_plain(t, records, stride, destination, block, t.bc1, kBc1Widths, bc1_residual, bc1_pack);
        return {};
    case 3:
        decode_plain(t, records, stride, destination, block, t.bc3, kBc3Widths, bc3_residual, bc3_pack);
        return {};
    case 4:
        decode_plain(t, records, stride, destination, block, t.bc4, kBc4Widths, bc4_residual, bc4_pack);
        return {};
    case 5:
        decode_plain(t, records, stride, destination, block, t.bc5, kBc5Widths, bc5_residual, bc5_pack);
        return {};
    case 6:
        return decode_bc6(t, records, stride, destination);
    default:
        return decode_bc7(t, records, stride, destination);
    }
}

namespace {

std::vector<std::uint8_t> synthetic_tables()
{
    std::vector<std::uint8_t> t;
    for (std::size_t m = 0; m < kModels; ++m) {
        const std::size_t g = m / 16;
        for (std::size_t b = 0; b < kBuckets; ++b) {
            const auto f = static_cast<std::uint16_t>(b < g + 2 ? 1u << (15 - g) : 0u);
            t.push_back(static_cast<std::uint8_t>(f));
            t.push_back(static_cast<std::uint8_t>(f >> 8));
        }
    }
    for (std::uint32_t v = 0; v < 256; ++v) {
        t.push_back(static_cast<std::uint8_t>(v & 0x77));
    }
    for (std::uint32_t v = 0; v < 64; ++v) {
        t.push_back(static_cast<std::uint8_t>(v));
    }
    for (std::uint32_t v = 0; v < 256; ++v) {
        t.push_back(static_cast<std::uint8_t>(v));
    }
    for (std::uint32_t v = 0; v < 64; ++v) {
        t.push_back(static_cast<std::uint8_t>((v & 7) | ((v >> 3) << 4)));
    }
    for (std::uint32_t v = 0; v < 64; ++v) {
        t.push_back(static_cast<std::uint8_t>(v));
    }
    for (std::uint32_t code = 0; code < 512; ++code) {
        const std::uint16_t ctrl = code == 0 ? 0x46 : 0;
        t.push_back(static_cast<std::uint8_t>(ctrl));
        t.push_back(static_cast<std::uint8_t>(ctrl >> 8));
    }
    t.resize(tables_size, 0);
    return t;
}

std::vector<std::uint8_t> transport_tables()
{
    std::vector<std::uint8_t> t;
    for (std::size_t m = 0; m < kModels; ++m) {
        const std::size_t g = m / 16;
        for (std::size_t b = 0; b < kBuckets; ++b) {
            const auto f = static_cast<std::uint16_t>(b < g + 2 ? 1u << (15 - g) : 0u);
            t.push_back(static_cast<std::uint8_t>(f));
            t.push_back(static_cast<std::uint8_t>(f >> 8));
        }
    }
    t.resize(t.size() + 256 + 64 + 256, 0);
    for (std::uint32_t v = 0; v < 64; ++v) {
        t.push_back(static_cast<std::uint8_t>((v & 7) | ((v >> 3) << 4)));
    }
    t.resize(tables_size, 0);
    return t;
}

std::vector<std::uint8_t> fragment(std::uint8_t width, std::span<const std::uint8_t> symbols)
{
    const std::uint32_t f = 1u << (15 - width);
    std::uint32_t x = kLow;
    std::vector<std::uint8_t> bytes;
    for (auto it = symbols.rbegin(); it != symbols.rend(); ++it) {
        const std::uint8_t s = *it;
        const std::uint32_t freq = s == 0 ? f - 1 : f;
        const std::uint32_t start = s == 0 ? 1 : static_cast<std::uint32_t>(s) * f;
        while (static_cast<std::uint64_t>(x) >= (static_cast<std::uint64_t>(freq) << 16)) {
            bytes.push_back(static_cast<std::uint8_t>(x));
            x >>= 8;
        }
        x = ((x / freq) << 16) + x % freq + start;
    }
    for (int k = 0; k < 4; ++k) {
        bytes.push_back(static_cast<std::uint8_t>(x >> (8 * k)));
    }
    std::reverse(bytes.begin(), bytes.end());
    std::vector<std::uint8_t> out{0};
    if (bytes.size() > 256) {
        out[0] |= 0x20;
        const std::size_t n = bytes.size() - 257;
        out.push_back(static_cast<std::uint8_t>(n));
        out.push_back(static_cast<std::uint8_t>(n >> 8));
    } else {
        out.push_back(static_cast<std::uint8_t>(bytes.size() - 1));
    }
    out.insert(out.end(), bytes.begin(), bytes.end());
    return out;
}

using Records = std::vector<std::vector<std::uint8_t>>;

std::vector<std::uint8_t> concat(const Records& records)
{
    std::vector<std::uint8_t> out;
    for (const auto& r : records) {
        out.insert(out.end(), r.begin(), r.end());
    }
    return out;
}

bool make_stream(const Tables& t, std::span<const std::uint8_t> widths, std::size_t per,
                 const Records& records, std::vector<std::uint8_t>& out)
{
    const std::size_t stride = widths.size() * per;
    std::vector<std::deque<std::uint8_t>> queues(widths.size());
    for (std::size_t j = 0; j < widths.size(); ++j) {
        std::vector<std::uint8_t> symbols;
        for (const auto& r : records) {
            symbols.insert(symbols.end(), r.begin() + static_cast<std::ptrdiff_t>(j * per),
                           r.begin() + static_cast<std::ptrdiff_t>(j * per + per));
        }
        auto bytes = fragment(widths[j], symbols);
        bytes.insert(bytes.end(), {0, 1, 0, 0});
        queues[j].assign(bytes.begin(), bytes.end());
    }
    out.clear();
    bool parked = false;
    std::vector<std::uint8_t> check(records.size() * stride);
    const auto fetch = [&](std::size_t j) -> int {
        if (queues[j].empty()) {
            parked = true;
            return -1;
        }
        const std::uint8_t b = queues[j].front();
        queues[j].pop_front();
        out.push_back(b);
        return b;
    };
    if (!run_lanes(t, widths, per, records.size(), fetch, check) || parked) {
        return false;
    }
    return check == concat(records);
}

std::uint8_t zigzag(int v)
{
    return static_cast<std::uint8_t>(v >= 0 ? 2 * v : -2 * v - 1);
}

std::pair<std::uint8_t, std::uint32_t> reference_decode(const Tables& t, std::uint8_t model,
                                                        std::uint32_t x)
{
    const auto& freq = t.freq[model];
    const auto& start = t.start[model];
    const std::uint32_t slot = x & 0xffff;
    std::size_t b = 0;
    for (std::size_t k = 1; k < kBuckets; ++k) {
        if (start[k] <= slot) {
            ++b;
        }
    }
    std::uint32_t symbol = 0;
    if (b == 1) {
        symbol = 1;
    } else if (b > 1) {
        symbol = (1u << (b - 1)) + (slot - start[b]) / freq[b];
    }
    const auto sym = static_cast<std::uint8_t>(symbol);
    std::uint32_t f = 0;
    std::uint32_t c = 0;
    if (sym == 0) {
        f = freq[0] - 1;
        c = 1;
    } else {
        std::size_t k = 0;
        for (std::uint32_t v = sym; v != 0; v >>= 1) {
            ++k;
        }
        f = freq[k];
        c = ((static_cast<std::uint32_t>(sym) - (1u << (k - 1))) * f + start[k]) & 0xffff;
    }
    return {sym, (x >> 16) * f - c + slot};
}

Records random_records(std::span<const std::uint8_t> widths, std::size_t per, std::size_t blocks,
                       bool bc7)
{
    std::uint64_t seed = 0x9e3779b97f4a7c15ull;
    Records records;
    for (std::size_t i = 0; i < blocks; ++i) {
        std::vector<std::uint8_t> r(widths.size() * per);
        for (std::size_t k = 0; k < r.size(); ++k) {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            const std::uint32_t alphabet = 1u << (widths[k / per] + 1);
            r[k] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(seed) % alphabet);
        }
        if (bc7) {
            r[30] = 0;
            r[31] = 0;
        }
        records.push_back(std::move(r));
    }
    return records;
}

std::array<std::uint8_t, 8> bc4_block(std::uint8_t e0, std::uint8_t e1, std::size_t add)
{
    std::uint64_t bits = 0;
    for (std::uint32_t p = 0; p < 16; ++p) {
        bits |= kBc4Code[(p + add) & 7] << (16 + 3 * p);
    }
    const std::uint64_t value = e0 | (static_cast<std::uint64_t>(e1) << 8) | bits;
    std::array<std::uint8_t, 8> out{};
    store64(out.data(), value);
    return out;
}

constexpr std::array<std::uint8_t, 88> kBc4Stream{
    0,    0,    0,    0,    0, 0, 0, 0, 5,    5,    5,    5,    4,    4,    4,    4,    1,    1,
    1,    1,    0x10, 0x10, 0x10, 0x10, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0x14, 5, 0x20, 0x68,
    0xb0, 0xf8, 3,    3,    2, 2, 0x24, 0x24, 0x24, 0x24, 2,    0,    0,    0,    0,    0,
    0,    0,    0,    0,    0, 0, 4, 4, 4,    4,    0x10, 0x10, 0x10, 0x10, 0,    0,    0,    0,
    0x20, 0x68, 0xb0, 0xf8, 0x24, 0x24, 0x24, 0x24, 0, 0, 0, 0, 0, 0, 0, 0,
};

class Suite {
public:
    explicit Suite(std::string& report) : report_(report) {}

    void check(std::string_view name, bool ok)
    {
        report_ += std::format("{} {}\n", ok ? "ok  " : "FAIL", name);
        if (!ok) {
            ++failures_;
        }
    }

    int failures() const noexcept { return failures_; }

private:
    std::string& report_;
    int failures_{};
};

TablesPtr must_parse(const std::vector<std::uint8_t>& bytes)
{
    auto parsed = parse_tables(bytes);
    return parsed ? std::move(*parsed) : TablesPtr{};
}

}

int self_test(std::string& report)
{
    Suite suite{report};
    const Tables* shipped = builtin_tables();
    suite.check("builtin tables parse", shipped != nullptr);
    const auto synthetic_bytes = synthetic_tables();
    const TablesPtr synthetic = must_parse(synthetic_bytes);
    suite.check("synthetic tables parse", synthetic != nullptr);
    const TablesPtr transport = must_parse(transport_tables());
    suite.check("transport tables parse", transport != nullptr);
    if (shipped == nullptr || synthetic == nullptr || transport == nullptr) {
        return suite.failures();
    }
    const Tables& t = *synthetic;

    {
        std::vector<std::uint8_t> b0{0, 0, zigzag(10), zigzag(-3)};
        for (std::uint32_t k = 0; k < 8; ++k) {
            b0.push_back(static_cast<std::uint8_t>(((2 * k) & 7) | (((2 * k + 1) & 7) << 3)));
        }
        std::vector<std::uint8_t> b1{0, 1, zigzag(1), zigzag(1)};
        for (int k = 0; k < 8; ++k) {
            b1.push_back(1 | (1 << 3));
        }
        std::vector<std::uint8_t> source;
        const bool streamed = make_stream(t, kBc4Lanes, 1, {b0, b1}, source);
        std::array<std::uint8_t, 16> out{};
        const bool decoded = streamed && decode(4, source, out, t).has_value();
        const auto first = bc4_block(138, 125, 0);
        const auto second = bc4_block(139, 126, 1);
        suite.check("bc4 blocks with and without references",
                    decoded && std::equal(first.begin(), first.end(), out.begin())
                        && std::equal(second.begin(), second.end(), out.begin() + 8));
    }

    {
        Records records;
        for (std::uint32_t i = 0; i < 400; ++i) {
            std::vector<std::uint8_t> r{0, 0, static_cast<std::uint8_t>(i * 7),
                                        static_cast<std::uint8_t>(i * 13)};
            for (std::uint32_t k = 0; k < 8; ++k) {
                r.push_back(static_cast<std::uint8_t>((i + k) * 5 % 64));
            }
            records.push_back(std::move(r));
        }
        std::vector<std::uint8_t> source;
        bool ok = make_stream(t, kBc4Lanes, 1, records, source);
        if (ok) {
            std::vector<std::uint8_t> got(records.size() * 12);
            std::size_t next = 0;
            const auto fetch = [&](std::size_t) -> int {
                return next < source.size() ? source[next++] : -1;
            };
            ok = run_lanes(t, kBc4Lanes, 1, records.size(), fetch, got).has_value()
                 && got == concat(records);
        }
        suite.check("lanes take bytes in schedule order with long fragments", ok);
    }

    {
        std::vector<std::uint8_t> s(32, 0);
        s[4] = zigzag(0x55);
        s[5] = zigzag(-1);
        s[6] = zigzag(3);
        s[10] = zigzag(0x7f >> 1);
        std::vector<std::uint8_t> source;
        const bool streamed = make_stream(t, kBc7Lanes, 2, {s}, source);
        std::array<std::uint8_t, 16> out{};
        const bool decoded = streamed && decode(7, source, out, t).has_value();
        Bits w;
        w.put(1u << 6, 7);
        constexpr std::array<std::pair<std::uint64_t, std::uint32_t>, 8> kFields{{
            {0x2a, 7}, {0x7f, 7}, {3, 7}, {0, 7}, {0, 7}, {0, 7}, {0x3f, 7}, {0, 7},
        }};
        for (const auto& [value, n] : kFields) {
            w.put(value, n);
        }
        w.put(1, 1);
        w.put(1, 1);
        suite.check("bc7 mode 6 block", decoded && out == w.bytes());
    }

    {
        Records records;
        for (std::uint8_t i = 0; i < 64; ++i) {
            records.push_back({0, 0, i, i, 1, 2, 3, 4, 5, 6, 7, 8});
        }
        std::vector<std::uint8_t> source;
        bool ok = make_stream(t, kBc4Lanes, 1, records, source);
        std::vector<std::uint8_t> out(64 * 8);
        ok = ok && decode(4, source, out, t).has_value();
        const std::span<const std::uint8_t> half{source.data(), source.size() / 2};
        ok = ok && !decode(4, half, out, t).has_value();
        std::vector<std::uint8_t> headers(8, 0x30);
        headers.insert(headers.end(), 8, 0xff);
        ok = ok && !decode(4, headers, std::span<std::uint8_t>{out.data(), 8}, t).has_value();
        ok = ok && !decode(4, {}, out, t).has_value();
        ok = ok && !decode(4, source, std::span<std::uint8_t>{out.data(), 12}, t).has_value();
        ok = ok && !decode(4, source, std::span<std::uint8_t>{}, t).has_value();
        std::vector<std::uint8_t> larger(65 * 8);
        ok = ok && !decode(4, source, larger, t).has_value();
        ok = ok && !decode(2, source, out, t).has_value();
        const std::vector<std::uint8_t> tiny(16, 0);
        ok = ok && !parse_tables(tiny).has_value();
        auto no_tiling = synthetic_bytes;
        no_tiling[0] = 1;
        ok = ok && !parse_tables(no_tiling).has_value();
        suite.check("malformed input is rejected", ok);
    }

    {
        std::uint64_t seed = 0x2545f4914f6cdd1dull;
        for (std::size_t round = 0; round < 300; ++round) {
            const std::size_t length = round * 7 % 900;
            std::vector<std::uint8_t> source(length);
            for (auto& b : source) {
                seed ^= seed << 13;
                seed ^= seed >> 7;
                seed ^= seed << 17;
                b = static_cast<std::uint8_t>(seed);
            }
            for (const int bc : {1, 3, 4, 5, 6, 7}) {
                std::vector<std::uint8_t> out(16 * (1 + round % 40));
                (void)decode(static_cast<std::uint8_t>(bc), source, out, t);
            }
        }
        suite.check("noise decodes without faults", true);
    }

    {
        auto narrow_bytes = synthetic_bytes;
        for (std::size_t m = 0; m < kModels; ++m) {
            const std::size_t used = m / 16 + 2;
            std::uint32_t rest = 1u << 16;
            for (std::size_t b = 1; b < used; ++b) {
                rest -= 1u << (b - 1);
            }
            for (std::size_t b = 0; b < used; ++b) {
                const auto f = static_cast<std::uint16_t>(b == 0 ? rest : 1u);
                narrow_bytes[(m * kBuckets + b) * 2] = static_cast<std::uint8_t>(f);
                narrow_bytes[(m * kBuckets + b) * 2 + 1] = static_cast<std::uint8_t>(f >> 8);
            }
        }
        const TablesPtr narrow = must_parse(narrow_bytes);
        bool ok = narrow != nullptr;
        for (const Tables* table : std::array<const Tables*, 3>{shipped, synthetic.get(), narrow.get()}) {
            if (table == nullptr) {
                continue;
            }
            for (std::size_t model = 0; model < kModels && ok; ++model) {
                const auto& start = table->start[model];
                for (std::uint32_t slot = 0; slot < (1u << 16); ++slot) {
                    std::size_t scan = 0;
                    for (std::size_t k = 1; k < kBuckets + 1; ++k) {
                        if (start[k] <= slot) {
                            ++scan;
                        }
                    }
                    if (bucket_of(*table, static_cast<std::uint8_t>(model), slot) != scan) {
                        ok = false;
                        break;
                    }
                }
            }
        }
        suite.check("bucket lookup matches a scan of the starts", ok);
    }

    {
        bool ok = true;
        for (const Tables* table : std::array<const Tables*, 2>{shipped, synthetic.get()}) {
            for (std::size_t model = 0; model < kModels && ok; ++model) {
                for (const std::uint32_t high : {0x100u, 0x1234u, 0xffffu}) {
                    for (std::uint32_t slot = 0; slot < (1u << 16); ++slot) {
                        const std::uint32_t x = (high << 16) | slot;
                        if (x <= kLow) {
                            continue;
                        }
                        Lane lane{};
                        lane.phase = 3;
                        lane.model = static_cast<std::uint8_t>(model);
                        lane.x = x;
                        lane.left = 1;
                        step(lane, *table);
                        const auto [symbol, next] =
                            reference_decode(*table, static_cast<std::uint8_t>(model), x);
                        if (lane.out != symbol || lane.x != next) {
                            ok = false;
                            break;
                        }
                    }
                }
            }
        }
        suite.check("payload step matches the reference decode", ok);
    }

    {
        const std::array<Layout, 6> layouts{
            Layout{kBc1Lanes, 1}, Layout{kBc3Lanes, 2}, Layout{kBc4Lanes, 1},
            Layout{kBc5Lanes, 2}, Layout{kBc6Lanes, 2}, Layout{kBc7Lanes, 2},
        };
        bool ok = true;
        for (const Layout& item : layouts) {
            for (const std::size_t blocks : {1u, 2u, 17u, 300u, 1000u}) {
                const Records records = random_records(item.widths, item.per, blocks, false);
                std::vector<std::uint8_t> source;
                if (!make_stream(t, item.widths, item.per, records, source)) {
                    ok = false;
                    continue;
                }
                std::vector<std::uint8_t> got(concat(records).size());
                std::size_t next = 0;
                const auto fetch = [&](std::size_t) -> int {
                    return next < source.size() ? source[next++] : -1;
                };
                ok = ok && run_lanes(t, item.widths, item.per, blocks, fetch, got).has_value()
                     && got == concat(records);
            }
        }
        suite.check("every layout round trips through the lanes", ok);
    }

    {
        bool ok = true;
        for (const Bc7Entry& entry : shipped->bc7) {
            const auto& ix = entry.ix;
            if (ix.bits[0] == 0) {
                ok = false;
                break;
            }
            const int limit = 1 << (ix.bits[0] - 1);
            bool empty = true;
            for (const auto v : ix.ix[1]) {
                empty = empty && v == 0;
            }
            if (!((ix.bits[1] != 0 || empty) && ix.ix[0][0] >= 0 && ix.ix[0][0] < limit)) {
                ok = false;
                break;
            }
        }
        suite.check("bc7 dictionary indices line up", ok);
    }

    {
        std::array<std::uint8_t, 16> out{};
        const bool decoded = decode(4, kBc4Stream, out, *transport).has_value();
        const auto first = bc4_block(138, 125, 0);
        const auto second = bc4_block(139, 126, 1);
        suite.check("packed bc4 transport stream",
                    decoded && std::equal(first.begin(), first.end(), out.begin())
                        && std::equal(second.begin(), second.end(), out.begin() + 8));
    }

    return suite.failures();
}

}
