#include <euphoria/formats/texture.hpp>
#include <euphoria/log/log.hpp>
#include <euphoria/shim/libraries.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>

namespace euphoria::shim {
namespace {

using HResult = std::int32_t;

constexpr HResult kOk = 0;
constexpr HResult kNotImpl = static_cast<HResult>(0x80004001u);
constexpr HResult kFail = static_cast<HResult>(0x80004005u);
constexpr HResult kInvalidArg = static_cast<HResult>(0x80070057u);

constexpr std::uint32_t kSwizzled = 0x100;
constexpr std::uint32_t kRowMajor = 1;
constexpr std::uint32_t kAllowRenderTarget = 1;
constexpr std::uint32_t kAllowDepthStencil = 2;
constexpr std::uint32_t kTexture1d = 2;
constexpr std::uint32_t kTexture2d = 3;
constexpr std::uint32_t kTexture3d = 4;
constexpr std::size_t kMips = 15;
constexpr std::uint64_t kTile = 64 * 1024;
constexpr std::uint32_t kLogLimit = 32;

struct ResourceDesc {
    std::uint32_t dimension;
    std::uint64_t alignment;
    std::uint64_t width;
    std::uint32_t height;
    std::uint16_t depth_or_array;
    std::uint16_t mips;
    std::uint32_t format;
    std::uint32_t samples;
    std::uint32_t quality;
    std::uint32_t layout;
    std::uint32_t flags;
};
static_assert(sizeof(ResourceDesc) == 0x38);
static_assert(offsetof(ResourceDesc, width) == 0x10);
static_assert(offsetof(ResourceDesc, depth_or_array) == 0x1c);
static_assert(offsetof(ResourceDesc, format) == 0x20);
static_assert(offsetof(ResourceDesc, layout) == 0x2c);
static_assert(offsetof(ResourceDesc, flags) == 0x30);

struct MipOut {
    std::uint64_t size;
    std::uint64_t offset;
    std::uint64_t slice_size;
    std::uint32_t pitch_texels;
    std::uint32_t pitch_bytes;
    std::uint32_t alignment;
    std::uint32_t padded[3];
    std::uint32_t elements[2];
    std::uint32_t texels[2];
    std::uint32_t depth_or_array;
    std::uint32_t samples;
    std::uint32_t swizzle;
    std::uint64_t bank_rotation[2];
    std::uint32_t slice_depth;
    std::uint32_t in_tail;
    std::uint32_t tail_position;
};
static_assert(sizeof(MipOut) == 0x70);
static_assert(offsetof(MipOut, offset) == 0x08);
static_assert(offsetof(MipOut, slice_size) == 0x10);
static_assert(offsetof(MipOut, pitch_bytes) == 0x1c);
static_assert(offsetof(MipOut, elements) == 0x30);
static_assert(offsetof(MipOut, swizzle) == 0x48);
static_assert(offsetof(MipOut, bank_rotation) == 0x50);
static_assert(offsetof(MipOut, in_tail) == 0x64);
static_assert(offsetof(MipOut, tail_position) == 0x68);

struct PlaneOut {
    std::uint32_t usage;
    std::uint64_t size;
    std::uint64_t slice_size;
    std::uint64_t offset;
    std::uint64_t alignment;
    std::uint32_t element_bytes;
    std::uint32_t standard_mips;
    std::uint32_t tail_mips;
    MipOut mips[kMips];
};
static_assert(sizeof(PlaneOut) == 0x6c8);
static_assert(offsetof(PlaneOut, element_bytes) == 0x28);
static_assert(offsetof(PlaneOut, tail_mips) == 0x30);
static_assert(offsetof(PlaneOut, mips) == 0x38);

struct LayoutOut {
    std::uint64_t size;
    std::uint64_t alignment;
    std::uint32_t mips;
    std::uint32_t planes;
    PlaneOut plane[4];
    std::uint32_t dimension;
};
static_assert(sizeof(LayoutOut) == 0x1b40);
static_assert(offsetof(LayoutOut, plane) == 0x18);
static_assert(offsetof(LayoutOut, dimension) == 0x1b38);

struct PackedMips {
    std::uint8_t standard;
    std::uint8_t packed;
    std::uint32_t tiles;
    std::uint32_t start;
};
static_assert(sizeof(PackedMips) == 12);
static_assert(offsetof(PackedMips, tiles) == 4);

struct SubresourceTiling {
    std::uint32_t width;
    std::uint16_t height;
    std::uint16_t depth;
    std::uint32_t start;
};
static_assert(sizeof(SubresourceTiling) == 12);

struct Computer {
    const void* const* vtable;
    std::atomic<std::uint32_t> refs;
    ResourceDesc desc;
    std::uint32_t mips;
    formats::Texture texture;
};

std::string describe(const ResourceDesc& d)
{
    return std::format("dimension {} width {} height {} depth {} mips {} format {} samples {} layout {:#x} flags {:#x}",
                       d.dimension, d.width, d.height, d.depth_or_array, d.mips, d.format, d.samples, d.layout, d.flags);
}

bool budget(std::atomic<std::uint32_t>& counter) noexcept
{
    return counter.fetch_add(1, std::memory_order_relaxed) < kLogLimit;
}

formats::Swizzle suggested(const ResourceDesc& desc) noexcept
{
    if (desc.dimension == kTexture1d) {
        return formats::Swizzle::LINEAR;
    }
    if ((desc.flags & kAllowDepthStencil) != 0) {
        return formats::Swizzle::SW_64KB_Z_X;
    }
    if ((desc.flags & kAllowRenderTarget) != 0) {
        return formats::Swizzle::SW_64KB_R_X;
    }
    return formats::Swizzle::SW_4KB_S;
}

HResult xg_suggest_swizzle_mode(const ResourceDesc* desc, std::uint32_t, std::uint32_t* out)
{
    if (desc == nullptr || out == nullptr) {
        return kInvalidArg;
    }
    *out = kSwizzled | suggested(*desc).value;
    return kOk;
}

std::optional<formats::Texture> make_texture(const ResourceDesc& desc)
{
    formats::TextureDesc t;
    switch (desc.dimension) {
    case kTexture1d:
        t.dimension = formats::Dimension::Tex1d;
        break;
    case kTexture2d:
        t.dimension = formats::Dimension::Tex2d;
        break;
    case kTexture3d:
        t.dimension = formats::Dimension::Tex3d;
        break;
    default:
        return std::nullopt;
    }
    if (desc.layout == 0) {
        t.swizzle = suggested(desc);
    } else if (desc.layout == kRowMajor) {
        t.swizzle = formats::Swizzle::LINEAR;
    } else if ((desc.layout & ~0xFFu) == kSwizzled) {
        t.swizzle = formats::Swizzle{desc.layout & 0xFFu};
    } else {
        return std::nullopt;
    }
    if (desc.width > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    t.width = static_cast<std::uint32_t>(desc.width);
    t.height = desc.height;
    t.depth_or_array = desc.depth_or_array;
    t.mips = desc.mips;
    t.format = desc.format;
    t.samples = desc.samples > 1 ? desc.samples : 1;
    auto texture = formats::Texture::create(t);
    if (!texture || texture->planes[0].layout.mips.size() > kMips) {
        return std::nullopt;
    }
    return std::move(*texture);
}

const void* const* computer_vtable() noexcept;

std::atomic<std::uint32_t> g_created{0};
std::atomic<std::uint32_t> g_failures{0};

HResult xg_create_texture_computer(const ResourceDesc* desc, Computer** out)
{
    if (desc == nullptr || out == nullptr) {
        return kInvalidArg;
    }
    const ResourceDesc copy = *desc;
    std::optional<formats::Texture> texture = make_texture(copy);
    if (!texture) {
        if (budget(g_failures)) {
            log::warn("xg: no texture layout for " + describe(copy));
        }
        return kInvalidArg;
    }
    if (budget(g_created)) {
        log::info(std::format("xg: texture computer width {} height {} format {} layout {:#x} size {}",
                              copy.width, copy.height, copy.format, copy.layout, texture->size));
    }
    auto* computer = new Computer{};
    computer->vtable = computer_vtable();
    computer->refs.store(1, std::memory_order_relaxed);
    computer->desc = copy;
    computer->mips = static_cast<std::uint32_t>(texture->planes[0].layout.mips.size());
    computer->texture = std::move(*texture);
    *out = computer;
    return kOk;
}

std::uint32_t tail_swizzle(formats::Swizzle swizzle) noexcept
{
    switch (swizzle.value) {
    case 2:
    case 6:
    case 10:
    case 18:
    case 22:
    case 26:
        return formats::Swizzle::SW_256B_D.value;
    case 23:
    case 27:
        return 3;
    case 24:
        return swizzle.value;
    default:
        return formats::Swizzle::SW_256B_S.value;
    }
}

MipOut mip_out(const ResourceDesc& desc, const formats::Layout& l, std::size_t i)
{
    const formats::Mip& m = l.mips[i];
    const std::uint64_t bpe = l.desc.element_bytes;
    const std::uint32_t block = l.desc.block;
    const bool three_d = desc.dimension == kTexture3d;
    const std::uint32_t array = desc.depth_or_array > 0 ? desc.depth_or_array : 1u;
    MipOut out{};
    out.pitch_texels = m.pitch * block;
    out.pitch_bytes = m.pitch * l.desc.element_bytes;
    out.padded[0] = m.pitch;
    out.padded[1] = m.rows;
    out.padded[2] = three_d ? m.depth_pitch : array;
    out.elements[0] = m.width;
    out.elements[1] = m.height;
    const std::uint32_t texel_width = static_cast<std::uint32_t>(desc.width >> i);
    const std::uint32_t texel_height = desc.height >> i;
    out.texels[0] = texel_width > 0 ? texel_width : 1u;
    out.texels[1] = texel_height > 0 ? texel_height : 1u;
    out.depth_or_array = three_d ? m.depth : array;
    out.samples = l.desc.samples;
    out.slice_depth = l.block[2];
    const std::uint64_t slice = std::uint64_t{m.pitch} * m.rows * bpe;
    const std::uint64_t depth = three_d ? m.depth_pitch : 1u;
    if (!m.tail) {
        out.offset = m.offset;
        out.slice_size = slice * l.desc.samples;
        out.size = out.slice_size * depth;
        out.alignment = static_cast<std::uint32_t>(l.alignment);
        out.swizzle = l.desc.swizzle.value;
    } else {
        out.in_tail = 1;
        out.alignment = 256;
        out.swizzle = tail_swizzle(l.desc.swizzle);
        if (l.thick) {
            out.slice_size = l.alignment / l.block[2];
            out.tail_position = m.tail->x | (m.tail->y << 11);
        } else {
            out.offset = m.tail->offset;
            out.slice_size = slice;
        }
        out.size = out.slice_size * depth;
    }
    return out;
}

std::uint32_t add_ref(Computer* self)
{
    return self->refs.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::uint32_t release(Computer* self)
{
    const std::uint32_t left = self->refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (left == 0) {
        delete self;
    }
    return left;
}

HResult get_layout(Computer* self, LayoutOut* out)
{
    if (out == nullptr) {
        return kInvalidArg;
    }
    std::memset(out, 0, sizeof(LayoutOut));
    const Computer& c = *self;
    out->size = c.texture.size;
    out->alignment = c.texture.alignment;
    out->mips = c.mips;
    out->planes = static_cast<std::uint32_t>(c.texture.planes.size());
    out->dimension = c.desc.dimension;
    const bool depth = (c.desc.flags & kAllowDepthStencil) != 0 && (c.texture.planes.size() > 1 || c.mips == 1);
    const std::size_t count = c.texture.planes.size() < 4 ? c.texture.planes.size() : 4;
    for (std::size_t index = 0; index < count; ++index) {
        const formats::Plane& p = c.texture.planes[index];
        const formats::Layout& l = p.layout;
        PlaneOut& plane = out->plane[index];
        plane.usage = p.stencil ? 8u : (depth ? 7u : 1u);
        plane.size = l.size;
        plane.slice_size = l.slice_size;
        plane.offset = p.offset;
        plane.alignment = l.alignment;
        plane.element_bytes = l.desc.element_bytes;
        plane.standard_mips = l.first_tail_mip;
        plane.tail_mips = c.mips - l.first_tail_mip;
        const std::size_t mips = l.mips.size() < kMips ? l.mips.size() : kMips;
        for (std::size_t i = 0; i < mips; ++i) {
            plane.mips[i] = mip_out(c.desc, l, i);
        }
    }
    return kOk;
}

struct CopyArgs {
    const formats::Layout* layout;
    std::uint64_t plane_offset;
    std::uint32_t mip;
    std::uint32_t slice;
    std::size_t tiled;
    std::size_t linear;
};

std::optional<CopyArgs> copy_args(const Computer& c, std::uint32_t plane, std::uint32_t subresource, std::uint32_t row, std::uint32_t depth)
{
    if (plane >= c.texture.planes.size()) {
        return std::nullopt;
    }
    const formats::Plane& p = c.texture.planes[plane];
    const formats::Layout& l = p.layout;
    const std::uint32_t mip = subresource % c.mips;
    const std::uint32_t slice = subresource / c.mips;
    if (mip >= l.mips.size()) {
        return std::nullopt;
    }
    const formats::Mip& m = l.mips[mip];
    const std::size_t bytes = std::size_t{m.width} * l.desc.element_bytes;
    const std::size_t depths = l.subresource_depth(mip);
    const std::size_t linear = (depths - 1) * depth + (std::size_t{m.height} - 1) * row + bytes;
    return CopyArgs{&l, p.offset, mip, slice, static_cast<std::size_t>(l.size), linear};
}

HResult result(const std::expected<void, formats::Error>& r) noexcept
{
    if (r) {
        return kOk;
    }
    return r.error() == formats::Error::Unsupported ? kNotImpl : kInvalidArg;
}

std::atomic<std::uint32_t> g_copy_failures{0};

void warn_copy(const Computer& c, const char* what, formats::Error error, std::uint32_t subresource)
{
    if (budget(g_copy_failures)) {
        log::warn(std::format("xg: copy {} a texture failed ({}) subresource {} {}",
                              what, formats::error_name(error), subresource, describe(c.desc)));
    }
}

HResult copy_into(Computer* self, std::uint8_t* tiled, std::uint32_t plane, std::uint32_t subresource,
                  const std::uint8_t* linear, std::uint32_t row, std::uint32_t depth)
{
    const Computer& c = *self;
    const std::optional<CopyArgs> a = copy_args(c, plane, subresource, row, depth);
    if (!a) {
        return kInvalidArg;
    }
    if (tiled == nullptr || linear == nullptr) {
        return kInvalidArg;
    }
    const auto r = a->layout->tile(std::span<std::uint8_t>(tiled + a->plane_offset, a->tiled), a->mip, a->slice,
                                   std::span<const std::uint8_t>(linear, a->linear), row, depth);
    if (!r) {
        warn_copy(c, "into", r.error(), subresource);
    }
    return result(r);
}

HResult copy_from(Computer* self, std::uint8_t* linear, std::uint32_t plane, std::uint32_t subresource,
                  std::uint32_t, const std::uint8_t* tiled, std::uint32_t row, std::uint32_t depth)
{
    const Computer& c = *self;
    const std::optional<CopyArgs> a = copy_args(c, plane, subresource, row, depth);
    if (!a) {
        return kInvalidArg;
    }
    if (tiled == nullptr || linear == nullptr) {
        return kInvalidArg;
    }
    const auto r = a->layout->detile(std::span<const std::uint8_t>(tiled + a->plane_offset, a->tiled), a->mip, a->slice,
                                     std::span<std::uint8_t>(linear, a->linear), row, depth);
    if (!r) {
        warn_copy(c, "out of", r.error(), subresource);
    }
    return result(r);
}

HResult tiling(Computer* self, std::uint32_t* tiles, PackedMips* packed, std::uint32_t (*shape)[3],
               std::uint32_t* count, std::uint32_t first, SubresourceTiling* out)
{
    const Computer& c = *self;
    const formats::Layout& l = c.texture.planes[0].layout;
    if (l.alignment != kTile) {
        return kFail;
    }
    if (l.thick) {
        return kNotImpl;
    }
    const std::uint32_t standard = l.first_tail_mip;
    const bool tail = standard < c.mips;
    std::uint64_t tail_tiles = 0;
    if (standard == 0) {
        tail_tiles = l.slice_size / kTile;
    } else if (standard < c.mips) {
        tail_tiles = l.mips[standard - 1].offset / kTile;
    }
    if (tiles != nullptr) {
        *tiles = static_cast<std::uint32_t>(l.size / kTile);
    }
    if (packed != nullptr) {
        *packed = PackedMips{static_cast<std::uint8_t>(standard), static_cast<std::uint8_t>(c.mips - standard),
                             static_cast<std::uint32_t>(tail_tiles), 0};
    }
    if (shape != nullptr) {
        (*shape)[0] = l.block[0] * l.desc.block;
        (*shape)[1] = l.block[1] * l.desc.block;
        (*shape)[2] = l.block[2];
    }
    if (count != nullptr) {
        const std::uint32_t slices = tail ? (l.slices < 2 ? l.slices : 2u) : l.slices;
        const std::uint64_t total = std::uint64_t{standard} * slices;
        std::uint64_t wanted = total > first ? total - first : 0;
        if (wanted > *count) {
            wanted = *count;
        }
        for (std::uint32_t i = 0; i < wanted && out != nullptr; ++i) {
            const std::uint32_t slice = (first + i) / standard;
            const std::uint32_t mip = (first + i) % standard;
            const formats::Mip& m = l.mips[mip];
            SubresourceTiling entry{};
            if (!(tail && slice > 0)) {
                entry.width = m.pitch / l.block[0];
                entry.height = static_cast<std::uint16_t>(m.rows / l.block[1]);
                entry.depth = 1;
                entry.start = static_cast<std::uint32_t>((std::uint64_t{slice} * l.slice_size + m.offset) / kTile);
            }
            out[i] = entry;
        }
        *count = static_cast<std::uint32_t>(wanted);
    }
    return kOk;
}

std::uint64_t element_offset(Computer* self, std::uint32_t plane, std::uint32_t mip, std::uint64_t x,
                             std::uint32_t y, std::uint32_t z, std::uint32_t, std::uint32_t* bit)
{
    if (bit != nullptr) {
        *bit = 0;
    }
    const Computer& c = *self;
    if (plane >= c.texture.planes.size()) {
        return 0;
    }
    const formats::Plane& p = c.texture.planes[plane];
    const std::uint32_t column = x > std::numeric_limits<std::uint32_t>::max()
        ? std::numeric_limits<std::uint32_t>::max()
        : static_cast<std::uint32_t>(x);
    const auto at = p.layout.offset(mip, column, y, z);
    if (!at) {
        static std::atomic<std::uint32_t> failures{0};
        if (budget(failures)) {
            log::warn(std::format("xg: texel outside its texture ({}) mip {} x {} y {} z {}",
                                  formats::error_name(at.error()), mip, x, y, z));
        }
        return 0;
    }
    return p.offset + *at;
}

template <std::uint32_t Slot>
HResult unused(Computer* self)
{
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
        log::warn(std::format("xg: unimplemented texture computer method {:#x} {}", Slot * 8, describe(self->desc)));
    }
    return kNotImpl;
}

template <typename Function>
const void* slot(Function function) noexcept
{
    return reinterpret_cast<const void*>(function);
}

const std::array<const void*, 13> kVtable{
    slot(&add_ref),
    slot(&release),
    slot(&get_layout),
    slot(&unused<3>),
    slot(&unused<4>),
    slot(&unused<5>),
    slot(&element_offset),
    slot(&unused<7>),
    slot(&copy_into),
    slot(&copy_from),
    slot(&tiling),
    slot(&unused<11>),
    slot(&unused<12>),
};

const void* const* computer_vtable() noexcept
{
    return kVtable.data();
}

const std::array<Entry, 2> kEntries{{
    {"XGCreateTextureComputer", 0, reinterpret_cast<Thunk>(&xg_create_texture_computer)},
    {"XGSuggestSwizzleMode", 0, reinterpret_cast<Thunk>(&xg_suggest_swizzle_mode)},
}};

const Library kLibrary{"xg", kEntries};

}

const Library& xg_library() noexcept
{
    return kLibrary;
}

}
