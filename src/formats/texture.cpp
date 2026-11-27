#include <euphoria/formats/texture.hpp>

#include <euphoria/log/log.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <limits>
#include <string>
#include <string_view>

namespace euphoria::formats {
namespace {

constexpr std::uint32_t kMaxMips = 16;
constexpr std::uint32_t kMaxExtent = 1u << 14;

constexpr std::uint32_t div_ceil(std::uint32_t value, std::uint32_t divisor) noexcept
{
    return value / divisor + (value % divisor != 0 ? 1u : 0u);
}

constexpr std::uint32_t round_up(std::uint32_t value, std::uint32_t multiple) noexcept
{
    return div_ceil(value, multiple) * multiple;
}

constexpr std::uint32_t dim(std::uint32_t texels, std::uint32_t mip, std::uint32_t block) noexcept
{
    return div_ceil(std::max(texels >> mip, 1u), block);
}

constexpr std::uint32_t padded_dim(std::uint32_t texels, std::uint32_t mip, std::uint32_t block) noexcept
{
    return div_ceil(div_ceil(std::max(texels, 1u), block), 1u << mip);
}

constexpr std::uint8_t kNone = 0xFF;

constexpr std::uint8_t ax(std::uint8_t index) noexcept { return index; }
constexpr std::uint8_t ay(std::uint8_t index) noexcept { return static_cast<std::uint8_t>(0x10 | index); }
constexpr std::uint8_t az(std::uint8_t index) noexcept { return static_cast<std::uint8_t>(0x20 | index); }

using MicroRow = std::array<std::uint8_t, 8>;

constexpr std::array<std::array<MicroRow, 5>, 3> kMicro{{
    {{
        {ax(0), ax(1), ax(2), ax(3), ay(0), ay(1), ay(2), ay(3)},
        {kNone, ax(0), ax(1), ax(2), ay(0), ay(1), ay(2), ax(3)},
        {kNone, kNone, ax(0), ax(1), ay(0), ay(1), ay(2), ax(2)},
        {kNone, kNone, kNone, ax(0), ay(0), ay(1), ax(1), ax(2)},
        {kNone, kNone, kNone, kNone, ay(0), ay(1), ax(0), ax(1)},
    }},
    {{
        {ax(0), ax(1), ax(2), ay(1), ay(0), ay(2), ax(3), ay(3)},
        {kNone, ax(0), ax(1), ax(2), ay(0), ay(1), ay(2), ax(3)},
        {kNone, kNone, ax(0), ax(1), ay(0), ay(1), ax(2), ay(2)},
        {kNone, kNone, kNone, ax(0), ay(0), ax(1), ax(2), ay(1)},
        {kNone, kNone, kNone, kNone, ax(0), ay(0), ax(1), ay(1)},
    }},
    {{
        {ax(0), ax(1), az(0), ay(0), az(1), ay(1), ax(2), az(2)},
        {kNone, ax(0), az(0), ay(0), az(1), ay(1), ax(1), az(2)},
        {kNone, kNone, ax(0), ay(0), az(0), ay(1), ax(1), az(1)},
        {kNone, kNone, kNone, ax(0), az(0), ay(0), ax(1), az(1)},
        {kNone, kNone, kNone, kNone, az(0), ay(0), ax(0), az(1)},
    }},
}};

constexpr std::array<std::array<std::uint32_t, 2>, 5> kBlock256x2d{{{16, 16}, {16, 8}, {8, 8}, {8, 4}, {4, 4}}};
constexpr std::array<std::array<std::uint32_t, 3>, 5> kBlock256x3d{{{8, 4, 8}, {4, 4, 8}, {4, 4, 4}, {4, 2, 4}, {2, 2, 4}}};
constexpr std::array<std::array<std::uint32_t, 3>, 5> kBlock1kx3d{{{16, 8, 8}, {8, 8, 8}, {8, 8, 4}, {8, 4, 4}, {4, 4, 4}}};

using Axes = std::array<std::vector<std::uint32_t>, 3>;

Axes make_pattern(std::size_t kind, std::uint32_t element_log2, std::uint32_t block_log2)
{
    Axes axes;
    const MicroRow& micro = kMicro[kind][element_log2];
    for (std::uint32_t bit = 0; bit < 8; ++bit) {
        const std::uint8_t code = micro[bit];
        if (code == kNone) {
            continue;
        }
        auto& to = axes[code >> 4];
        const std::size_t index = code & 0x0F;
        if (to.size() <= index) {
            to.resize(index + 1, 0);
        }
        to[index] = bit;
    }
    constexpr std::array<std::size_t, 3> kThickOrder{1, 0, 2};
    constexpr std::array<std::size_t, 2> kThinOrder{1, 0};
    std::size_t step = 0;
    for (std::uint32_t bit = 8; bit < block_log2; ++bit, ++step) {
        const std::size_t axis = kind == 2 ? kThickOrder[step % kThickOrder.size()] : kThinOrder[step % kThinOrder.size()];
        axes[axis].push_back(bit);
    }
    return axes;
}

std::vector<std::uint32_t> spread(const std::vector<std::uint32_t>& bits)
{
    const std::size_t count = std::size_t{1} << bits.size();
    std::vector<std::uint32_t> out(count, 0);
    for (std::size_t c = 0; c < count; ++c) {
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < bits.size(); ++i) {
            value += static_cast<std::uint32_t>((c >> i) & 1) << bits[i];
        }
        out[c] = value;
    }
    return out;
}

bool in_extent(std::uint32_t value) noexcept
{
    return value >= 1 && value <= kMaxExtent;
}

bool checked_mul(std::size_t a, std::size_t b, std::size_t& out) noexcept
{
    if (a != 0 && b > std::numeric_limits<std::size_t>::max() / a) {
        return false;
    }
    out = a * b;
    return true;
}

bool checked_add(std::size_t a, std::size_t b, std::size_t& out) noexcept
{
    if (b > std::numeric_limits<std::size_t>::max() - a) {
        return false;
    }
    out = a + b;
    return true;
}

struct Run {
    std::size_t tiled;
    std::size_t linear;
    std::size_t length;
};

template <bool Detile>
void copy_run(const std::uint8_t* src, std::uint8_t* dst, std::size_t tiled, std::size_t linear, std::size_t length) noexcept
{
    if constexpr (Detile) {
        std::memcpy(dst + linear, src + tiled, length);
    } else {
        std::memcpy(dst + tiled, src + linear, length);
    }
}

}

const char* error_name(Error error) noexcept
{
    switch (error) {
    case Error::Invalid:
        return "invalid";
    case Error::Unsupported:
        return "unsupported";
    case Error::TooSmall:
        return "too small";
    }
    return "unknown";
}

std::expected<Layout, Error> Layout::create(const Desc& d)
{
    const std::optional<std::uint32_t> block_log2 = d.swizzle.block_log2();
    if (!block_log2) {
        return std::unexpected(Error::Invalid);
    }
    const std::uint32_t s = d.swizzle.value;
    const bool three_d_mode = s == 0 || s == 5 || s == 9 || s == 17 || s == 21 || (s >= 24 && s <= 27);
    const bool valid = in_extent(d.width) && in_extent(d.height) && in_extent(d.depth)
        && d.mips >= 1 && d.mips <= kMaxMips
        && std::has_single_bit(d.element_bytes) && d.element_bytes <= 16
        && (d.block == 1 || d.block == 4)
        && std::has_single_bit(d.samples) && d.samples <= 8
        && (d.samples == 1 || (d.dimension == Dimension::Tex2d && d.mips == 1))
        && (d.dimension != Dimension::Tex1d || d.height == 1)
        && (d.dimension != Dimension::Tex3d || three_d_mode);
    if (!valid) {
        return std::unexpected(Error::Invalid);
    }
    if (*block_log2 == 0) {
        return linear(d);
    }
    return tiled(d, *block_log2);
}

Layout Layout::linear(const Desc& d)
{
    const std::uint64_t bpe = d.element_bytes;
    const std::uint32_t align = 256 / d.element_bytes;
    const bool three_d = d.dimension == Dimension::Tex3d;
    Layout l;
    l.mips.reserve(d.mips);
    std::uint64_t offset = 0;
    for (std::uint32_t i = d.mips; i-- > 0;) {
        const std::uint32_t h = padded_dim(d.height, i, d.block);
        const std::uint32_t pitch = round_up(padded_dim(d.width, i, d.block), align);
        Mip m;
        m.width = dim(d.width, i, d.block);
        m.height = dim(d.height, i, d.block);
        m.depth = three_d ? dim(d.depth, i, 1) : 1;
        m.pitch = pitch;
        m.rows = h;
        m.depth_pitch = three_d ? d.depth : 1;
        m.offset = offset;
        l.mips.push_back(m);
        offset += std::uint64_t{pitch} * h * bpe;
    }
    std::reverse(l.mips.begin(), l.mips.end());
    l.desc = d;
    l.block = {align, 1, 1};
    l.thick = false;
    l.slices = d.depth;
    l.slice_size = offset;
    l.size = offset * d.depth;
    l.alignment = 256;
    l.first_tail_mip = d.mips;
    return l;
}

Layout Layout::tiled(const Desc& d, std::uint32_t block_log2)
{
    const std::uint32_t element_log2 = static_cast<std::uint32_t>(std::countr_zero(d.element_bytes));
    const std::uint64_t bpe = d.element_bytes;
    const std::uint64_t block_bytes = std::uint64_t{1} << block_log2;
    const bool thick = d.dimension == Dimension::Tex3d && (d.swizzle.standard() || d.swizzle.display());

    std::array<std::uint32_t, 3> block{};
    if (thick) {
        const std::uint32_t amp = (block_log2 - 10) / 3;
        const std::uint32_t rest = (block_log2 - 10) % 3;
        const auto& b = kBlock1kx3d[element_log2];
        block = {b[0] << amp, b[1] << (amp + rest / 2), b[2] << (amp + (rest != 0 ? 1u : 0u))};
    } else {
        const std::uint32_t samples_log2 = static_cast<std::uint32_t>(std::countr_zero(d.samples));
        const std::uint32_t elements_log2 = block_log2 - element_log2 - samples_log2;
        const bool wide = (samples_log2 & 1) == 0 || (block_log2 & 1) != 0;
        const std::uint32_t w = (elements_log2 + (wide ? 1u : 0u)) / 2;
        block = {1u << w, 1u << (elements_log2 - w), 1};
    }

    const std::uint32_t slices = round_up(d.depth, block[2]);
    const bool three_d = d.dimension == Dimension::Tex3d;
    const auto mip_depth = [&](std::uint32_t i) { return thick ? padded_dim(d.depth, i, 1) : 1u; };
    const auto depth_of = [&](std::uint32_t i) { return three_d ? dim(d.depth, i, 1) : 1u; };
    const auto width_of = [&](std::uint32_t i) { return dim(d.width, i, d.block); };
    const auto height_of = [&](std::uint32_t i) { return dim(d.height, i, d.block); };

    std::vector<Mip> mips;
    mips.reserve(d.mips);
    std::uint32_t first_tail_mip = d.mips;
    std::uint64_t slice_size = 0;

    if (d.mips == 1) {
        Mip m;
        m.width = width_of(0);
        m.height = height_of(0);
        m.depth = depth_of(0);
        m.pitch = round_up(width_of(0), block[0]);
        m.rows = round_up(height_of(0), block[1]);
        m.depth_pitch = three_d ? slices : 1;
        m.offset = 0;
        mips.push_back(m);
        slice_size = std::uint64_t{m.pitch} * m.rows * bpe * d.samples;
    } else {
        std::array<std::uint32_t, 3> tail_max = block;
        if (thick) {
            switch (block_log2 % 3) {
            case 0:
                tail_max[1] >>= 1;
                break;
            case 1:
                tail_max[0] >>= 1;
                break;
            default:
                tail_max[2] >>= 1;
                break;
            }
        } else {
            tail_max[0] >>= 1;
        }
        const std::uint32_t effective = thick ? block_log2 - (block_log2 - 8) / 3 : block_log2;
        std::uint32_t max_in_tail = 0;
        if (effective == 8) {
            max_in_tail = 0;
        } else if (effective >= 9 && effective <= 11) {
            max_in_tail = 1 + (1u << (effective - 9));
        } else {
            max_in_tail = effective - 4;
        }
        std::array<std::uint32_t, 3> fixed = tail_max;
        if (d.swizzle.z_order() && element_log2 <= 1) {
            const auto& b = kBlock256x2d[element_log2];
            fixed[0] /= b[0] / kBlock256x2d[2][0];
            fixed[1] /= b[1] / kBlock256x2d[2][1];
        }
        std::vector<std::uint64_t> sizes;
        for (std::uint32_t i = 0; i < d.mips; ++i) {
            const std::uint32_t w = padded_dim(d.width, i, d.block);
            const std::uint32_t h = padded_dim(d.height, i, d.block);
            if (w <= fixed[0] && h <= fixed[1] && d.mips - i <= max_in_tail) {
                first_tail_mip = i;
                slice_size += block_bytes / block[2];
                break;
            }
            const std::uint32_t pitch = round_up(w, block[0]);
            const std::uint32_t rows = round_up(h, block[1]);
            const std::uint32_t depth = round_up(mip_depth(i), block[2]);
            const std::uint64_t one = std::uint64_t{pitch} * rows * bpe;
            slice_size += one;
            sizes.push_back(one * block[2]);
            Mip m;
            m.width = width_of(i);
            m.height = height_of(i);
            m.depth = depth_of(i);
            m.pitch = pitch;
            m.rows = rows;
            m.depth_pitch = three_d ? depth : 1;
            m.offset = 0;
            mips.push_back(m);
        }
        std::uint64_t offset = first_tail_mip < d.mips ? block_bytes : 0;
        for (std::uint32_t i = first_tail_mip; i-- > 0;) {
            mips[i].offset = offset;
            offset += sizes[i];
        }
        std::array<std::uint32_t, 2> micro{};
        if (thick) {
            micro = {kBlock256x3d[element_log2][0], kBlock256x3d[element_log2][1]};
        } else {
            micro = kBlock256x2d[element_log2];
        }
        std::uint32_t pitch = tail_max[0];
        std::uint32_t rows = tail_max[1];
        for (std::uint32_t i = first_tail_mip; i < d.mips; ++i) {
            const std::uint32_t slot = max_in_tail - 1 - (i - first_tail_mip);
            const std::uint32_t tail_offset = slot > 6 ? (16u << slot) : (slot << 8);
            const auto bits = [tail_offset](std::uint32_t from) {
                std::uint32_t value = 0;
                for (std::uint32_t b = 0; b < 6; ++b) {
                    value += ((tail_offset >> (from + 2 * b)) & 1u) << b;
                }
                return value;
            };
            std::uint32_t x = bits(9);
            std::uint32_t y = bits(8);
            if ((block_log2 & 1) != 0) {
                std::swap(x, y);
                if ((element_log2 & 1) != 0) {
                    y = (y << 1) | (x & 1);
                    x >>= 1;
                }
            }
            Mip m;
            m.width = width_of(i);
            m.height = height_of(i);
            m.depth = depth_of(i);
            m.pitch = pitch;
            m.rows = rows;
            m.depth_pitch = thick ? round_up(mip_depth(i), kBlock256x3d[element_log2][2]) : 1;
            m.offset = 0;
            m.tail = Tail{tail_offset, x * micro[0], y * micro[1]};
            mips.push_back(m);
            pitch = std::max(pitch >> 1, micro[0]);
            rows = std::max(rows >> 1, micro[1]);
        }
    }

    const std::uint64_t slab = thick ? block[2] : 1;
    slice_size *= slab;
    const std::uint32_t slabs = slices / block[2];
    const bool addressable = !d.swizzle.is_xor() && d.samples == 1;

    Layout l;
    if (addressable) {
        const std::size_t kind = thick ? 2 : (d.swizzle.display() ? 1 : 0);
        const Axes axes = make_pattern(kind, element_log2, block_log2);
        for (std::size_t axis = 0; axis < 3; ++axis) {
            l.pattern_[axis] = spread(axes[axis]);
        }
    }
    l.desc = d;
    l.block = block;
    l.thick = thick;
    l.slices = slabs;
    l.slice_size = slice_size;
    l.size = slice_size * slabs;
    l.alignment = block_bytes;
    l.first_tail_mip = first_tail_mip;
    l.mips = std::move(mips);
    return l;
}

bool Layout::addressable() const noexcept
{
    return desc.swizzle == Swizzle::LINEAR || !pattern_[0].empty();
}

std::pair<std::uint32_t, std::uint32_t> Layout::split(std::uint32_t z) const noexcept
{
    if (thick) {
        return {z / block[2], z % block[2]};
    }
    return {z, 0};
}

std::expected<std::uint64_t, Error> Layout::offset(std::uint32_t mip, std::uint32_t x, std::uint32_t y, std::uint32_t z) const
{
    if (mip >= mips.size()) {
        return std::unexpected(Error::Invalid);
    }
    const Mip& m = mips[mip];
    const std::uint32_t depth = desc.dimension == Dimension::Tex3d ? m.depth : slices;
    const bool inside = x < m.width && y < m.height && z < depth;
    if (!inside) {
        return std::unexpected(Error::Invalid);
    }
    if (!addressable()) {
        return std::unexpected(Error::Unsupported);
    }
    const auto [slice, inner] = split(z);
    const std::uint64_t base = std::uint64_t{slice} * slice_size + m.offset;
    if (desc.swizzle == Swizzle::LINEAR) {
        const std::uint64_t row_index = std::uint64_t{y} * m.pitch + x;
        return base + row_index * desc.element_bytes;
    }
    return base + row(m, y, inner) + column(m, x);
}

std::uint64_t Layout::row(const Mip& m, std::uint32_t y, std::uint32_t z) const noexcept
{
    const std::uint32_t bw = block[0];
    const std::uint32_t bh = block[1];
    const int log2 = std::countr_zero(alignment);
    const std::uint32_t shifted = y + (m.tail ? m.tail->y : 0u);
    const std::uint64_t blocks = std::uint64_t{shifted / bh} * (m.pitch / bw);
    return (blocks << log2) + (pattern_[1][shifted % bh] | pattern_[2][z]);
}

std::uint64_t Layout::column(const Mip& m, std::uint32_t x) const noexcept
{
    const std::uint32_t bw = block[0];
    const std::uint32_t shifted = x + (m.tail ? m.tail->x : 0u);
    return (std::uint64_t{shifted / bw} << std::countr_zero(alignment)) + pattern_[0][shifted % bw];
}

std::uint32_t Layout::subresource_depth(std::uint32_t mip) const noexcept
{
    return mip < mips.size() ? mips[mip].depth : 0;
}

std::expected<void, Error> Layout::detile(std::span<const std::uint8_t> tiled,
                                          std::uint32_t mip,
                                          std::uint32_t slice,
                                          std::span<std::uint8_t> linear,
                                          std::size_t row_pitch,
                                          std::size_t depth_pitch) const
{
    return walk<true>(tiled.data(), tiled.size(), linear.data(), linear.size(), mip, slice, row_pitch, depth_pitch);
}

std::expected<void, Error> Layout::tile(std::span<std::uint8_t> tiled,
                                        std::uint32_t mip,
                                        std::uint32_t slice,
                                        std::span<const std::uint8_t> linear,
                                        std::size_t row_pitch,
                                        std::size_t depth_pitch) const
{
    return walk<false>(linear.data(), linear.size(), tiled.data(), tiled.size(), mip, slice, row_pitch, depth_pitch);
}

template <bool Detile>
std::expected<void, Error> Layout::walk(const std::uint8_t* src,
                                        std::size_t src_size,
                                        std::uint8_t* dst,
                                        std::size_t dst_size,
                                        std::uint32_t mip,
                                        std::uint32_t slice,
                                        std::size_t row_pitch,
                                        std::size_t depth_pitch) const
{
    if (mip >= mips.size()) {
        return std::unexpected(Error::Invalid);
    }
    const Mip& m = mips[mip];
    const std::uint32_t depth = m.depth;
    std::uint32_t first = slice;
    if (desc.dimension == Dimension::Tex3d) {
        if (slice != 0) {
            return std::unexpected(Error::Invalid);
        }
        first = 0;
    }
    if (first >= slices) {
        return std::unexpected(Error::Invalid);
    }
    if (!addressable()) {
        return std::unexpected(Error::Unsupported);
    }
    const std::size_t n = desc.element_bytes;
    const std::size_t bytes = std::size_t{m.width} * n;
    std::size_t depth_offset = 0;
    std::size_t row_offset = 0;
    std::size_t need = 0;
    if (!checked_mul(std::size_t{depth} - 1, depth_pitch, depth_offset)
        || !checked_mul(std::size_t{m.height} - 1, row_pitch, row_offset)
        || !checked_add(depth_offset, row_offset, need)
        || !checked_add(need, bytes, need)) {
        return std::unexpected(Error::TooSmall);
    }
    const std::size_t tiled_len = Detile ? src_size : dst_size;
    const std::size_t linear_len = Detile ? dst_size : src_size;
    if (tiled_len < size || linear_len < need || row_pitch < bytes) {
        return std::unexpected(Error::TooSmall);
    }
    const bool is_linear = desc.swizzle == Swizzle::LINEAR;
    std::vector<Run> runs;
    if (!is_linear) {
        std::size_t first_linear = 0;
        std::size_t first_tiled = static_cast<std::size_t>(column(m, 0));
        for (std::uint32_t x = 1; x < m.width; ++x) {
            const std::size_t at_tiled = static_cast<std::size_t>(column(m, x));
            const std::size_t at = std::size_t{x} * n;
            if (at_tiled != first_tiled + (at - first_linear)) {
                runs.push_back({first_tiled, first_linear, at - first_linear});
                first_tiled = at_tiled;
                first_linear = at;
            }
        }
        runs.push_back({first_tiled, first_linear, bytes - first_linear});
    }
    for (std::uint32_t z = 0; z < depth; ++z) {
        const auto [s, inner] = split(first + z);
        const std::uint64_t base = std::uint64_t{s} * slice_size + m.offset;
        for (std::uint32_t y = 0; y < m.height; ++y) {
            const std::size_t linear_row = std::size_t{z} * depth_pitch + std::size_t{y} * row_pitch;
            if (is_linear) {
                const std::uint64_t tiled_row = base + std::uint64_t{y} * m.pitch * n;
                copy_run<Detile>(src, dst, static_cast<std::size_t>(tiled_row), linear_row, bytes);
            } else {
                const std::size_t tiled_row = static_cast<std::size_t>(base + row(m, y, inner));
                for (const Run& run : runs) {
                    copy_run<Detile>(src, dst, tiled_row + run.tiled, linear_row + run.linear, run.length);
                }
            }
        }
    }
    return {};
}

std::optional<Format> format_info(std::uint32_t dxgi) noexcept
{
    const auto in = [dxgi](std::uint32_t low, std::uint32_t high) { return dxgi >= low && dxgi <= high; };
    if (in(1, 4)) {
        return Format{16, 1, false};
    }
    if (in(9, 18)) {
        return Format{8, 1, false};
    }
    if (in(19, 22) || in(44, 47)) {
        return Format{4, 1, true};
    }
    if (in(23, 43) || dxgi == 67 || in(87, 93) || dxgi == 116 || dxgi == 117) {
        return Format{4, 1, false};
    }
    if (in(48, 59) || dxgi == 85 || dxgi == 86 || dxgi == 115) {
        return Format{2, 1, false};
    }
    if (in(118, 120)) {
        return Format{2, 1, true};
    }
    if (in(60, 65)) {
        return Format{1, 1, false};
    }
    if (in(70, 72) || in(79, 81)) {
        return Format{8, 4, false};
    }
    if (in(73, 78) || in(82, 84) || in(94, 99)) {
        return Format{16, 4, false};
    }
    return std::nullopt;
}

std::expected<Texture, Error> Texture::create(const TextureDesc& desc)
{
    const std::optional<Format> f = format_info(desc.format);
    if (!f) {
        return std::unexpected(Error::Invalid);
    }
    const std::uint32_t depth3d = desc.dimension == Dimension::Tex3d ? desc.depth_or_array : 1;
    std::uint32_t mips = desc.mips;
    if (mips == 0) {
        mips = static_cast<std::uint32_t>(std::bit_width(std::max({desc.width, desc.height, depth3d, 1u})));
    }
    const auto plane = [&](std::uint32_t bytes) {
        Desc d;
        d.dimension = desc.dimension;
        d.width = desc.width;
        d.height = desc.height;
        d.depth = desc.depth_or_array;
        d.mips = mips;
        d.element_bytes = bytes;
        d.block = f->block;
        d.samples = std::max(desc.samples, 1u);
        d.swizzle = desc.swizzle;
        return d;
    };
    std::expected<Layout, Error> first = Layout::create(plane(f->bytes));
    if (!first) {
        return std::unexpected(first.error());
    }
    Texture t;
    t.planes.push_back(Plane{0, false, std::move(*first)});
    if (f->stencil) {
        std::expected<Layout, Error> stencil = Layout::create(plane(1));
        if (!stencil) {
            return std::unexpected(stencil.error());
        }
        const std::uint64_t align = stencil->alignment;
        const std::uint64_t offset = (t.planes[0].layout.size + align - 1) / align * align;
        t.planes.push_back(Plane{offset, true, std::move(*stencil)});
    }
    const Plane& last = t.planes.back();
    t.size = last.offset + last.layout.size;
    t.alignment = 1;
    for (const Plane& p : t.planes) {
        t.alignment = std::max(t.alignment, p.layout.alignment);
    }
    return t;
}

namespace {

struct Checker {
    int failures{0};

    bool expect(bool ok, std::string_view what)
    {
        if (!ok) {
            ++failures;
            log::warn(std::format("texture selftest: mismatch: {}", what));
        }
        return ok;
    }
};

Desc make(std::uint32_t w, std::uint32_t h, std::uint32_t mips, std::uint32_t bpe, Swizzle swizzle)
{
    Desc d;
    d.dimension = Dimension::Tex2d;
    d.width = w;
    d.height = h;
    d.depth = 1;
    d.mips = mips;
    d.element_bytes = bpe;
    d.block = 1;
    d.samples = 1;
    d.swizzle = swizzle;
    return d;
}

std::string describe(const Desc& d)
{
    return std::format("{}x{}x{} mips {} bpe {} block {} swizzle {}",
                       d.width, d.height, d.depth, d.mips, d.element_bytes, d.block, d.swizzle.value);
}

void check_extents(Checker& c)
{
    for (const Swizzle swizzle : {Swizzle::LINEAR, Swizzle::SW_64KB_D_X}) {
        const auto largest = Layout::create(make(kMaxExtent, kMaxExtent, 15, 16, swizzle));
        if (c.expect(largest.has_value(), "largest extent accepted")) {
            c.expect(largest->size >= std::uint64_t{kMaxExtent} * kMaxExtent * 16, "largest extent size");
        }
        const std::array<std::array<std::uint32_t, 2>, 3> bad{{{kMaxExtent + 1, 1}, {1, kMaxExtent + 1}, {0xFFFFFFFFu, 0xFFFFFFFFu}}};
        for (const auto& [w, h] : bad) {
            const auto r = Layout::create(make(w, h, 1, 16, swizzle));
            c.expect(!r && r.error() == Error::Invalid, std::format("extent {}x{} refused", w, h));
        }
        Desc deep = make(1, 1, 1, 16, swizzle);
        deep.depth = 0xFFFFFFFFu;
        const auto r = Layout::create(deep);
        c.expect(!r && r.error() == Error::Invalid, "deep refused");
    }
}

void check_blocks(Checker& c)
{
    for (const std::uint32_t log2 : {8u, 12u, 16u}) {
        const Swizzle swizzle = log2 == 8 ? Swizzle::SW_256B_S : (log2 == 12 ? Swizzle::SW_4KB_S : Swizzle::SW_64KB_D);
        for (std::uint32_t e = 0; e < 5; ++e) {
            const auto l = Layout::create(make(1, 1, 1, 1u << e, swizzle));
            if (!c.expect(l.has_value(), std::format("block layout {} {}", log2, e))) {
                continue;
            }
            const auto [w, h, d] = l->block;
            c.expect((std::uint64_t{w} * h * d << e) == (std::uint64_t{1} << log2), std::format("block bytes {} {}", log2, e));
            c.expect(l->pattern()[0].size() == w && l->pattern()[1].size() == h, std::format("pattern span {} {}", log2, e));
        }
    }
    const auto l = Layout::create(make(64, 64, 1, 16, Swizzle::SW_64KB_D));
    if (c.expect(l.has_value(), "64x64 bc7 block")) {
        c.expect(l->block == std::array<std::uint32_t, 3>{64, 64, 1}, "64x64 block dims");
        c.expect(l->size == 65536 && l->alignment == 65536, "64x64 size and alignment");
    }
}

bool round_trip_case(Checker& c, const Desc& d)
{
    const auto created = Layout::create(d);
    if (!c.expect(created.has_value(), "round trip layout " + describe(d))) {
        return false;
    }
    const Layout& l = *created;
    std::vector<bool> seen(static_cast<std::size_t>(l.size), false);
    std::vector<std::uint8_t> tiled(static_cast<std::size_t>(l.size), 0);
    const std::size_t n = d.element_bytes;
    const std::uint32_t slices = d.dimension == Dimension::Tex3d ? 1 : d.depth;
    for (std::uint32_t slice = 0; slice < slices; ++slice) {
        for (std::uint32_t mip = 0; mip < d.mips; ++mip) {
            const Mip& m = l.mips[mip];
            const std::size_t active = std::size_t{m.width} * n;
            const std::size_t row = active + 13;
            const std::size_t plane = row * m.height + 11;
            std::vector<std::uint8_t> linear(plane * m.depth);
            for (std::size_t i = 0; i < linear.size(); ++i) {
                linear[i] = static_cast<std::uint8_t>(i * 7 + std::size_t{mip} * 13 + slice);
            }
            const std::string where = std::format("{} mip {} slice {}", describe(d), mip, slice);
            if (!c.expect(l.tile(tiled, mip, slice, linear, row, plane).has_value(), "tile " + where)) {
                return false;
            }
            std::vector<std::uint8_t> back(linear.size(), 0xCC);
            if (!c.expect(l.detile(tiled, mip, slice, back, row, plane).has_value(), "detile " + where)) {
                return false;
            }
            for (std::uint32_t z = 0; z < m.depth; ++z) {
                const std::size_t base = std::size_t{z} * plane;
                for (std::uint32_t y = 0; y < m.height; ++y) {
                    const std::size_t start = base + std::size_t{y} * row;
                    if (!c.expect(std::equal(back.begin() + start, back.begin() + start + active, linear.begin() + start), "rows back " + where)) {
                        return false;
                    }
                    if (!c.expect(std::all_of(back.begin() + start + active, back.begin() + start + row, [](std::uint8_t b) { return b == 0xCC; }), "row padding untouched " + where)) {
                        return false;
                    }
                    for (std::uint32_t x = 0; x < m.width; ++x) {
                        const auto at_result = l.offset(mip, x, y, slice + z);
                        if (!c.expect(at_result.has_value(), "offset " + where)) {
                            return false;
                        }
                        const std::size_t at = static_cast<std::size_t>(*at_result);
                        const std::size_t source = start + std::size_t{x} * n;
                        if (!c.expect(at + n <= seen.size(), "offset inside " + where)
                            || !c.expect(std::equal(tiled.begin() + at, tiled.begin() + at + n, linear.begin() + source), "tiled element " + where)
                            || !c.expect(at % n == 0, "offset aligned " + where)
                            || !c.expect(!seen[at], std::format("element ({},{},{}) twice {}", x, y, z, where))) {
                            return false;
                        }
                        seen[at] = true;
                    }
                }
                if (!c.expect(std::all_of(back.begin() + base + plane - 11, back.begin() + base + plane, [](std::uint8_t b) { return b == 0xCC; }), "plane padding untouched " + where)) {
                    return false;
                }
            }
        }
    }
    return true;
}

void check_round_trips(Checker& c)
{
    std::vector<Desc> cases{
        make(64, 64, 7, 16, Swizzle::SW_64KB_D),
        make(100, 60, 7, 4, Swizzle::SW_64KB_D),
        make(250, 175, 8, 8, Swizzle::SW_4KB_S),
        make(33, 17, 6, 1, Swizzle::SW_4KB_D),
        make(40, 9, 3, 2, Swizzle::SW_64KB_S),
        make(30, 20, 5, 4, Swizzle::LINEAR),
        make(83, 31, 6, 1, Swizzle::SW_256B_S),
    };
    Desc bc = make(109, 71, 6, 16, Swizzle::SW_256B_D);
    bc.block = 4;
    cases.push_back(bc);
    Desc array = make(70, 70, 4, 16, Swizzle::SW_4KB_S);
    array.depth = 3;
    cases.push_back(array);
    Desc volume = make(64, 32, 7, 4, Swizzle::SW_4KB_S);
    volume.dimension = Dimension::Tex3d;
    volume.depth = 16;
    cases.push_back(volume);
    for (const Desc& d : cases) {
        round_trip_case(c, d);
    }
}

void check_chunks(Checker& c)
{
    for (const Swizzle swizzle : {Swizzle::SW_256B_S, Swizzle::SW_256B_D, Swizzle::SW_4KB_S, Swizzle::SW_4KB_D, Swizzle::SW_64KB_S, Swizzle::SW_64KB_D}) {
        for (const std::uint32_t bpe : {1u, 2u, 4u, 8u, 16u}) {
            Desc d = make(73, 39, 7, bpe, swizzle);
            d.depth = 2;
            const auto created = Layout::create(d);
            if (!c.expect(created.has_value(), "chunk layout " + describe(d))) {
                continue;
            }
            const Layout& l = *created;
            const std::size_t n = bpe;
            std::vector<std::uint8_t> tiled(static_cast<std::size_t>(l.size));
            for (std::uint64_t i = 0; i < l.size; ++i) {
                tiled[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((i * 0x9E3779B9ull) >> 24);
            }
            std::vector<std::uint8_t> retiled(tiled.size(), 0xBB);
            std::vector<std::uint8_t> expected_tiled = retiled;
            bool ok = true;
            for (std::uint32_t slice = 0; slice < l.slices && ok; ++slice) {
                for (std::uint32_t mip = 0; mip < l.mips.size() && ok; ++mip) {
                    const Mip& m = l.mips[mip];
                    const std::size_t bytes = std::size_t{m.width} * n;
                    const std::size_t pitch = bytes + 3;
                    const std::size_t len = (std::size_t{m.height} - 1) * pitch + bytes;
                    std::vector<std::uint8_t> linear(len, 0xCC);
                    std::vector<std::uint8_t> expected = linear;
                    for (std::uint32_t y = 0; y < m.height; ++y) {
                        for (std::uint32_t x = 0; x < m.width; ++x) {
                            const std::size_t src = static_cast<std::size_t>(l.offset(mip, x, y, slice).value_or(0));
                            const std::size_t dst = std::size_t{y} * pitch + std::size_t{x} * n;
                            std::memcpy(expected.data() + dst, tiled.data() + src, n);
                            std::memcpy(expected_tiled.data() + src, tiled.data() + src, n);
                        }
                    }
                    const std::string where = std::format("{} mip {} slice {}", describe(d), mip, slice);
                    ok = c.expect(l.detile(tiled, mip, slice, linear, pitch, len).has_value(), "chunk detile " + where)
                        && c.expect(linear == expected, "chunk detile bytes " + where)
                        && c.expect(l.tile(retiled, mip, slice, expected, pitch, len).has_value(), "chunk tile " + where);
                }
            }
            if (ok) {
                c.expect(retiled == expected_tiled, "chunk retile " + describe(d));
            }
        }
    }
}

void expect_offset(Checker& c, const Layout& l, std::uint32_t mip, std::uint32_t x, std::uint32_t y, std::uint32_t z, std::uint64_t want)
{
    const auto got = l.offset(mip, x, y, z);
    c.expect(got.has_value() && *got == want,
             std::format("offset {} ({},{},{}) of {} = {} want {}", mip, x, y, z, describe(l.desc), got.value_or(~0ull), want));
}

void check_hand_addresses(Checker& c)
{
    if (const auto l = Layout::create(make(128, 128, 1, 4, Swizzle::SW_64KB_D)); c.expect(l.has_value(), "128x128 64KB_D")) {
        expect_offset(c, *l, 0, 1, 0, 0, 1u << 2);
        expect_offset(c, *l, 0, 4, 0, 0, 1u << 6);
        expect_offset(c, *l, 0, 0, 4, 0, 1u << 7);
        expect_offset(c, *l, 0, 0, 8, 0, 1u << 8);
        expect_offset(c, *l, 0, 64, 64, 0, (1u << 15) | (1u << 14));
    }
    if (const auto l = Layout::create(make(32, 32, 1, 16, Swizzle::SW_4KB_S)); c.expect(l.has_value(), "32x32 4KB_S")) {
        c.expect(l->block == std::array<std::uint32_t, 3>{16, 16, 1}, "32x32 4KB_S block");
        expect_offset(c, *l, 0, 0, 1, 0, 1u << 4);
        expect_offset(c, *l, 0, 1, 0, 0, 1u << 6);
        expect_offset(c, *l, 0, 16, 0, 0, 4096);
        expect_offset(c, *l, 0, 0, 16, 0, 8192);
    }
}

void check_mip_tail(Checker& c)
{
    if (const auto l = Layout::create(make(64, 64, 9, 16, Swizzle::SW_64KB_D)); c.expect(l.has_value(), "bc7 64KB_D chain")) {
        c.expect(l->first_tail_mip == 1, "64KB_D first tail mip");
        c.expect(l->size == 2 * 65536, "64KB_D size");
        c.expect(l->mips[0].offset == 65536, "64KB_D mip 0 offset");
        c.expect(l->mips[1].tail == Tail{32768, 32, 0}, "64KB_D mip 1 tail");
        expect_offset(c, *l, 1, 0, 0, 0, 32768);
    }
    if (const auto l = Layout::create(make(64, 64, 9, 16, Swizzle::SW_4KB_S)); c.expect(l.has_value(), "bc7 4KB_S chain")) {
        c.expect(l->first_tail_mip == 3, "4KB_S first tail mip");
        c.expect(l->mips[2].offset == 4096, "4KB_S mip 2 offset");
        c.expect(l->mips[1].offset == 8192, "4KB_S mip 1 offset");
        c.expect(l->mips[0].offset == 8192 + 16384, "4KB_S mip 0 offset");
    }
}

void check_bc7_texture(Checker& c)
{
    TextureDesc desc;
    desc.dimension = Dimension::Tex2d;
    desc.width = 256;
    desc.height = 256;
    desc.depth_or_array = 1;
    desc.mips = 0;
    desc.format = 98;
    desc.samples = 1;
    desc.swizzle = Swizzle::SW_64KB_D;
    const auto t = Texture::create(desc);
    if (!c.expect(t.has_value(), "bc7 texture")) {
        return;
    }
    const Layout& l = t->planes[0].layout;
    c.expect(t->planes.size() == 1, "bc7 planes");
    c.expect(t->size == 131072 && t->alignment == 65536, "bc7 size and alignment");
    c.expect(l.mips.size() == 9, "bc7 mip count");
    c.expect(l.first_tail_mip == 1 && l.mips.size() - l.first_tail_mip == 8, "bc7 standard and tail mips");
    c.expect(l.desc.element_bytes == 16 && l.desc.block == 4, "bc7 element");
    c.expect(l.mips[0].offset == 65536 && l.mips[0].pitch * l.desc.element_bytes == 1024, "bc7 mip 0");
    c.expect(l.mips[1].tail && l.mips[1].tail->offset == 32768, "bc7 mip 1 tail");
}

void check_depth_stencil(Checker& c)
{
    TextureDesc desc;
    desc.dimension = Dimension::Tex2d;
    desc.width = 1920;
    desc.height = 1080;
    desc.depth_or_array = 1;
    desc.mips = 1;
    desc.format = 20;
    desc.samples = 1;
    desc.swizzle = Swizzle::SW_64KB_Z_X;
    const auto t = Texture::create(desc);
    if (!c.expect(t.has_value() && t->planes.size() == 2, "depth stencil planes")) {
        return;
    }
    c.expect(t->planes[0].layout.size == 1920ull * 1152 * 4, "depth plane size");
    c.expect(t->planes[1].offset == t->planes[0].layout.size, "stencil plane offset");
    c.expect(t->planes[1].layout.size == 2048ull * 1280, "stencil plane size");
    c.expect(!t->planes[0].layout.addressable(), "depth plane not addressable");
}

void check_overflow(Checker& c)
{
    const auto layout = Layout::create(make(4, 2, 1, 4, Swizzle::LINEAR));
    if (c.expect(layout.has_value(), "overflow layout")) {
        std::vector<std::uint8_t> tiled(static_cast<std::size_t>(layout->size), 0);
        std::array<std::uint8_t, 32> linear{};
        const std::size_t huge = std::numeric_limits<std::size_t>::max();
        const auto d = layout->detile(tiled, 0, 0, linear, huge, 0);
        c.expect(!d && d.error() == Error::TooSmall, "detile huge row pitch");
        const auto t = layout->tile(tiled, 0, 0, linear, huge, 0);
        c.expect(!t && t.error() == Error::TooSmall, "tile huge row pitch");
    }
    Desc v = make(4, 2, 1, 4, Swizzle::LINEAR);
    v.dimension = Dimension::Tex3d;
    v.depth = 3;
    const auto volume = Layout::create(v);
    if (c.expect(volume.has_value(), "volume layout")) {
        std::vector<std::uint8_t> tiled(static_cast<std::size_t>(volume->size), 0);
        std::array<std::uint8_t, 96> linear{};
        const auto d = volume->detile(tiled, 0, 0, linear, 16, std::numeric_limits<std::size_t>::max());
        c.expect(!d && d.error() == Error::TooSmall, "detile huge depth pitch");
    }
}

void check_bad(Checker& c)
{
    const auto zero = Layout::create(make(0, 4, 1, 4, Swizzle::SW_4KB_S));
    c.expect(!zero && zero.error() == Error::Invalid, "zero width refused");
    const auto twelve = Layout::create(make(4, 4, 1, 12, Swizzle::SW_4KB_S));
    c.expect(!twelve && twelve.error() == Error::Invalid, "12-byte element refused");
    const auto mode = Layout::create(make(4, 4, 1, 4, Swizzle{3}));
    c.expect(!mode && mode.error() == Error::Invalid, "mode 3 refused");
    const auto l = Layout::create(make(64, 64, 1, 4, Swizzle::SW_4KB_S));
    if (c.expect(l.has_value(), "64x64 4KB_S")) {
        std::vector<std::uint8_t> out(64 * 64 * 4, 0);
        std::array<std::uint8_t, 100> small{};
        const auto d = l->detile(small, 0, 0, out, 256, 0);
        c.expect(!d && d.error() == Error::TooSmall, "small tiled refused");
        const auto o = l->offset(0, 64, 0, 0);
        c.expect(!o && o.error() == Error::Invalid, "outside offset refused");
    }
}

}

bool texture_selftest()
{
    Checker c;
    check_extents(c);
    check_blocks(c);
    check_round_trips(c);
    check_chunks(c);
    check_hand_addresses(c);
    check_mip_tail(c);
    check_bc7_texture(c);
    check_depth_stencil(c);
    check_overflow(c);
    check_bad(c);
    if (c.failures == 0) {
        log::info("texture selftest: passed");
    } else {
        log::warn(std::format("texture selftest: {} failures", c.failures));
    }
    return c.failures == 0;
}

}
