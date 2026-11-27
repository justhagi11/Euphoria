#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace openscarlett::zlib_rfc1950 {

constexpr uint32_t kMaxBits = 15u;
constexpr uint32_t kMaxLitLenSyms = 288u;
constexpr uint32_t kMaxDistSyms = 32u;

inline uint32_t ComputeAdler32(const uint8_t* data, size_t len) noexcept {
    constexpr uint32_t kModAdler = 65521u;
    uint32_t s1 = 1u;
    uint32_t s2 = 0u;
    size_t i = 0u;
    while (i < len) {
        const size_t chunk = std::min<size_t>(len - i, 5552u);
        for (size_t k = 0u; k < chunk; ++k) {
            s1 += data[i + k];
            s2 += s1;
        }
        s1 %= kModAdler;
        s2 %= kModAdler;
        i += chunk;
    }
    return (s2 << 16u) | s1;
}

struct BitStream {
    const uint8_t* data = nullptr;
    size_t size = 0u;
    size_t byte_pos = 0u;
    uint64_t bit_buf = 0ull;
    uint32_t bit_cnt = 0u;

    bool EnsureBits(uint32_t n) noexcept {
        while (bit_cnt < n) {
            if (byte_pos >= size) return false;
            bit_buf |= (static_cast<uint64_t>(data[byte_pos++]) << bit_cnt);
            bit_cnt += 8u;
        }
        return true;
    }

    bool ReadBits(uint32_t n, uint32_t& out) noexcept {
        if (n == 0u) {
            out = 0u;
            return true;
        }
        if (n > 32u || !EnsureBits(n)) return false;
        out = static_cast<uint32_t>(bit_buf & ((1ull << n) - 1ull));
        bit_buf >>= n;
        bit_cnt -= n;
        return true;
    }

    void AlignToByte() noexcept {
        const uint32_t drop = bit_cnt & 7u;
        bit_buf >>= drop;
        bit_cnt -= drop;
    }

    void FlushBufferedFullBytes() noexcept {
        AlignToByte();
        while (bit_cnt >= 8u && byte_pos > 0u) {
            --byte_pos;
            bit_cnt -= 8u;
        }
        bit_buf = 0ull;
        bit_cnt = 0u;
    }
};

struct HuffTable {
    std::array<uint16_t, kMaxBits + 1u> count{};
    std::array<uint16_t, kMaxLitLenSyms> symbols{};
    uint32_t num_syms = 0u;

    bool Build(const uint8_t* lengths, uint32_t n, bool allow_empty) noexcept {
        count.fill(0u);
        num_syms = 0u;
        for (uint32_t i = 0u; i < n; ++i) {
            if (lengths[i] > kMaxBits) return false;
            if (lengths[i] > 0u) {
                ++count[lengths[i]];
                ++num_syms;
            }
        }
        if (num_syms == 0u) {
            return allow_empty;
        }
        int32_t left = 1;
        for (uint32_t bits = 1u; bits <= kMaxBits; ++bits) {
            left <<= 1;
            left -= static_cast<int32_t>(count[bits]);
            if (left < 0) return false;
        }
        if (left > 0 && num_syms > 1u) {
            return false;
        }
        std::array<uint16_t, kMaxBits + 1u> offsets{};
        uint16_t off = 0u;
        for (uint32_t bits = 1u; bits <= kMaxBits; ++bits) {
            offsets[bits] = off;
            off = static_cast<uint16_t>(off + count[bits]);
        }
        for (uint32_t sym = 0u; sym < n; ++sym) {
            if (lengths[sym] > 0u) {
                symbols[offsets[lengths[sym]]++] = static_cast<uint16_t>(sym);
            }
        }
        return true;
    }

    bool Decode(BitStream& bs, uint32_t& out_sym) const noexcept {
        if (num_syms == 0u) return false;
        int32_t code = 0;
        int32_t first = 0;
        int32_t index = 0;
        for (uint32_t len = 1u; len <= kMaxBits; ++len) {
            uint32_t bit = 0u;
            if (!bs.ReadBits(1u, bit)) return false;
            code |= static_cast<int32_t>(bit);
            const int32_t cnt = static_cast<int32_t>(count[len]);
            if (code - cnt < first) {
                out_sym = symbols[static_cast<size_t>(index + (code - first))];
                return true;
            }
            index += cnt;
            first += cnt;
            first <<= 1;
            code <<= 1;
        }
        return false;
    }
};

inline bool BuildFixedTrees(HuffTable& litlen, HuffTable& dist) noexcept {
    std::array<uint8_t, kMaxLitLenSyms> ll{};
    for (uint32_t i = 0u; i <= 143u; ++i) ll[i] = 8u;
    for (uint32_t i = 144u; i <= 255u; ++i) ll[i] = 9u;
    for (uint32_t i = 256u; i <= 279u; ++i) ll[i] = 7u;
    for (uint32_t i = 280u; i <= 287u; ++i) ll[i] = 8u;
    std::array<uint8_t, kMaxDistSyms> dl{};
    dl.fill(5u);
    return litlen.Build(ll.data(), 288u, false) && dist.Build(dl.data(), 32u, false);
}

inline bool DecodeDynamicTrees(BitStream& bs, HuffTable& litlen, HuffTable& dist) noexcept {
    uint32_t hlit_raw = 0u, hdist_raw = 0u, hclen_raw = 0u;
    if (!bs.ReadBits(5u, hlit_raw) || !bs.ReadBits(5u, hdist_raw) || !bs.ReadBits(4u, hclen_raw)) {
        return false;
    }
    const uint32_t hlit = hlit_raw + 257u;
    const uint32_t hdist = hdist_raw + 1u;
    const uint32_t hclen = hclen_raw + 4u;
    if (hlit > 286u || hdist > 30u || hclen > 19u) {
        return false;
    }

    static constexpr std::array<uint8_t, 19> kOrder = {
        16u, 17u, 18u, 0u, 8u, 7u, 9u, 6u, 10u, 5u, 11u, 4u, 12u, 3u, 13u, 2u, 14u, 1u, 15u};
    std::array<uint8_t, 19> cl_lengths{};
    for (uint32_t i = 0u; i < hclen; ++i) {
        uint32_t v = 0u;
        if (!bs.ReadBits(3u, v)) return false;
        cl_lengths[kOrder[i]] = static_cast<uint8_t>(v);
    }

    HuffTable cl_tree{};
    if (!cl_tree.Build(cl_lengths.data(), 19u, false)) {
        return false;
    }

    const uint32_t total = hlit + hdist;
    std::array<uint8_t, kMaxLitLenSyms + kMaxDistSyms> lengths{};
    uint32_t idx = 0u;
    while (idx < total) {
        uint32_t sym = 0u;
        if (!cl_tree.Decode(bs, sym)) return false;
        if (sym < 16u) {
            lengths[idx++] = static_cast<uint8_t>(sym);
        } else if (sym == 16u) {
            if (idx == 0u) return false;
            uint32_t extra = 0u;
            if (!bs.ReadBits(2u, extra)) return false;
            const uint32_t rep = 3u + extra;
            if (idx + rep > total) return false;
            const uint8_t prev = lengths[idx - 1u];
            for (uint32_t r = 0u; r < rep; ++r) lengths[idx++] = prev;
        } else if (sym == 17u) {
            uint32_t extra = 0u;
            if (!bs.ReadBits(3u, extra)) return false;
            const uint32_t rep = 3u + extra;
            if (idx + rep > total) return false;
            for (uint32_t r = 0u; r < rep; ++r) lengths[idx++] = 0u;
        } else if (sym == 18u) {
            uint32_t extra = 0u;
            if (!bs.ReadBits(7u, extra)) return false;
            const uint32_t rep = 11u + extra;
            if (idx + rep > total) return false;
            for (uint32_t r = 0u; r < rep; ++r) lengths[idx++] = 0u;
        } else {
            return false;
        }
    }

    if (lengths[256u] == 0u) {
        return false;
    }
    return litlen.Build(lengths.data(), hlit, false) &&
           dist.Build(lengths.data() + hlit, hdist, true);
}

inline bool DecodeHuffmanBlock(
    BitStream& bs,
    const HuffTable& litlen,
    const HuffTable& dist,
    size_t max_out_size,
    std::vector<uint8_t>& out) {
    static constexpr std::array<uint16_t, 29> kLenBase = {
        3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
        35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr std::array<uint8_t, 29> kLenExtra = {
        0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
        3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr std::array<uint16_t, 30> kDistBase = {
        1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
        257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr std::array<uint8_t, 30> kDistExtra = {
        0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
        7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

    for (;;) {
        uint32_t sym = 0u;
        if (!litlen.Decode(bs, sym)) return false;
        if (sym < 256u) {
            if (out.size() >= max_out_size) return false;
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256u) {
            return true;
        } else if (sym <= 285u) {
            const size_t lidx = sym - 257u;
            uint32_t lextra = 0u;
            if (!bs.ReadBits(kLenExtra[lidx], lextra)) return false;
            const size_t match_len = static_cast<size_t>(kLenBase[lidx]) + lextra;

            uint32_t dsym = 0u;
            if (!dist.Decode(bs, dsym) || dsym >= 30u) return false;
            uint32_t dextra = 0u;
            if (!bs.ReadBits(kDistExtra[dsym], dextra)) return false;
            const size_t distance = static_cast<size_t>(kDistBase[dsym]) + dextra;

            if (distance == 0u || distance > out.size() || match_len > max_out_size - out.size()) {
                return false;
            }
            const size_t copy_start = out.size() - distance;
            for (size_t i = 0u; i < match_len; ++i) {
                out.push_back(out[copy_start + i]);
            }
        } else {
            return false;
        }
    }
}

inline bool Decompress(
    const uint8_t* src,
    size_t src_size,
    size_t max_out_size,
    std::vector<uint8_t>& out) {
    out.clear();
    if (!src || src_size < 6u) return false;

    const uint8_t cmf = src[0];
    const uint8_t flg = src[1];
    const uint32_t cm = cmf & 0x0Fu;
    const uint32_t cinfo = (cmf >> 4u) & 0x0Fu;
    const uint32_t fdict = (flg >> 5u) & 1u;
    const uint32_t fcheck = (static_cast<uint32_t>(cmf) << 8u) | static_cast<uint32_t>(flg);
    if (cm != 8u || cinfo > 7u || fdict != 0u || (fcheck % 31u) != 0u) {
        return false;
    }

    BitStream bs{};
    bs.data = src + 2u;
    bs.size = src_size - 6u;
    if (max_out_size > 0u && max_out_size <= 64u * 1024u * 1024u) {
        out.reserve(std::min<size_t>(max_out_size, 1024u * 1024u));
    }

    uint32_t bfinal = 0u;
    do {
        uint32_t btype = 0u;
        if (!bs.ReadBits(1u, bfinal) || !bs.ReadBits(2u, btype)) {
            out.clear();
            return false;
        }
        if (btype == 0u) {
            bs.FlushBufferedFullBytes();
            if (4u > bs.size - bs.byte_pos) {
                out.clear();
                return false;
            }
            const uint16_t len = static_cast<uint16_t>(bs.data[bs.byte_pos]) |
                                 static_cast<uint16_t>(static_cast<uint16_t>(bs.data[bs.byte_pos + 1u]) << 8u);
            const uint16_t nlen = static_cast<uint16_t>(bs.data[bs.byte_pos + 2u]) |
                                  static_cast<uint16_t>(static_cast<uint16_t>(bs.data[bs.byte_pos + 3u]) << 8u);
            bs.byte_pos += 4u;
            if ((len ^ 0xFFFFu) != nlen) {
                out.clear();
                return false;
            }
            if (len > bs.size - bs.byte_pos || len > max_out_size - out.size()) {
                out.clear();
                return false;
            }
            out.insert(out.end(), bs.data + bs.byte_pos, bs.data + bs.byte_pos + len);
            bs.byte_pos += len;
        } else if (btype == 1u) {
            HuffTable ll{}, dt{};
            if (!BuildFixedTrees(ll, dt) || !DecodeHuffmanBlock(bs, ll, dt, max_out_size, out)) {
                out.clear();
                return false;
            }
        } else if (btype == 2u) {
            HuffTable ll{}, dt{};
            if (!DecodeDynamicTrees(bs, ll, dt) || !DecodeHuffmanBlock(bs, ll, dt, max_out_size, out)) {
                out.clear();
                return false;
            }
        } else {
            out.clear();
            return false;
        }
    } while (bfinal == 0u);

    bs.FlushBufferedFullBytes();
    if (bs.byte_pos != bs.size) {
        out.clear();
        return false;
    }

    const uint8_t* tr = src + (src_size - 4u);
    const uint32_t expected_adler =
        (static_cast<uint32_t>(tr[0]) << 24u) |
        (static_cast<uint32_t>(tr[1]) << 16u) |
        (static_cast<uint32_t>(tr[2]) << 8u) |
        static_cast<uint32_t>(tr[3]);
    const uint32_t actual_adler = ComputeAdler32(out.data(), out.size());
    if (actual_adler != expected_adler) {
        out.clear();
        return false;
    }
    return true;
}

}  // namespace openscarlett::zlib_rfc1950
