#include <euphoria/shader/gfx10/native.hpp>

#include <euphoria/formats/inflate.hpp>
#include <euphoria/shader/gfx10/dxbc.hpp>

#include <algorithm>
#include <cstring>
#include <limits>

namespace euphoria::shader::gfx10 {
namespace {

constexpr std::size_t kXdatOffset = 0x18;
constexpr std::size_t kMaxXdatEntries = 64;
constexpr std::uint32_t kKindNative = 0x0d;
constexpr std::uint32_t kXbchVersion = 3;
constexpr std::size_t kXbchHeader = 0x18;
constexpr std::uint32_t kFssmVersion = 0x101;
constexpr std::size_t kFssmHeader = 0x10;
constexpr std::uint64_t kGraphicsSlotCount = 6;

struct Slot {
    HwStage stage;
    std::size_t at;
};

constexpr std::array<Slot, 8> kSlots{{
    {HwStage::Ls, 0x08},
    {HwStage::Hs, 0x10},
    {HwStage::Es, 0x18},
    {HwStage::Gs, 0x20},
    {HwStage::Vs, 0x28},
    {HwStage::Ps, 0x30},
    {HwStage::Ms, 0x48},
    {HwStage::Cs, 0x60},
}};

constexpr std::uint64_t kAbsent = 0xDEADBEEFDEADBEEFull;
constexpr std::uint64_t kRecordSize = 0x54;
constexpr std::size_t kRecordFlags = 0x14;
constexpr std::uint32_t kFlagWave32 = 1u << 5;
constexpr std::size_t kRecordRegs = 0x50;
constexpr std::size_t kRecordLayout = 0x28;
constexpr std::size_t kRecordUsed = 0x30;
constexpr std::size_t kRecordCounts = 0x38;
constexpr std::uint64_t kEntrySize = 20;
constexpr std::uint32_t kMaxSemantics = 32;
constexpr std::size_t kPsInputs = 0x24;
constexpr std::size_t kPsInputCount = 0x124;
constexpr std::size_t kPsInputAddr = 0x134;
constexpr std::size_t kPsDbShaderControl = 0x138;
constexpr std::size_t kPsPatchSize = 0x148;
constexpr std::size_t kPsPatches = 0x14c;
constexpr std::size_t kPatchSlotDwords = 10;
constexpr std::size_t kPatchFull = 9;
constexpr std::size_t kPatchBytes = 4 + 10 * kPatchSlotDwords * 4;
constexpr std::size_t kMrts = 8;
constexpr std::size_t kVsInputs = 0x24;
constexpr std::size_t kVsInputCount = 0xc4;
constexpr std::size_t kVsOutputs = 0xc8;
constexpr std::size_t kVsOutputCount = 0x188;
constexpr std::size_t kCsRsrc2 = 0x24;
constexpr std::size_t kCsThreads = 0x28;
constexpr std::size_t kRecordLds = 0x4c;
constexpr std::size_t kMsClipCntl = 0x194;
constexpr std::size_t kMsInfo = 0x448;
constexpr std::uint64_t kMaxUnpacked = 16ull << 20;

using Bytes = std::span<const std::uint8_t>;

template <typename T>
using Result = std::expected<T, ShaderError>;

ShaderError malformed(const char* what) noexcept
{
    return ShaderError::Malformed(what);
}

std::optional<Bytes> bytes(Bytes data, std::uint64_t offset, std::uint64_t length) noexcept
{
    if (offset > data.size() || length > data.size() - offset) {
        return std::nullopt;
    }
    return data.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(length));
}

bool has_magic(Bytes data, std::size_t at, const char (&magic)[5]) noexcept
{
    return at <= data.size() && data.size() - at >= 4 && std::memcmp(data.data() + at, magic, 4) == 0;
}

std::uint32_t le32(Bytes entry, std::size_t at) noexcept
{
    return static_cast<std::uint32_t>(entry[at]) | (static_cast<std::uint32_t>(entry[at + 1]) << 8)
         | (static_cast<std::uint32_t>(entry[at + 2]) << 16) | (static_cast<std::uint32_t>(entry[at + 3]) << 24);
}

Result<Bytes> semantics(Bytes data, std::size_t at, std::size_t size, std::size_t count_at)
{
    const auto count = u32_at(data, count_at);
    if (!count) {
        return std::unexpected(count.error());
    }
    if (*count > kMaxSemantics) {
        return std::unexpected(malformed("implausible semantic count"));
    }
    const auto entries = bytes(data, at, static_cast<std::uint64_t>(size) * *count);
    if (!entries) {
        return std::unexpected(malformed("stage data out of bounds"));
    }
    return *entries;
}

Result<PsIo> ps_io(Bytes data, std::size_t code_length)
{
    const auto entries = semantics(data, kPsInputs, 8, kPsInputCount);
    if (!entries) {
        return std::unexpected(entries.error());
    }
    PsIo io;
    for (std::size_t at = 0; at + 8 <= entries->size(); at += 8) {
        const Bytes e = entries->subspan(at, 8);
        io.inputs.push_back(PsInput{
            .usage = static_cast<Usage>(e[0]),
            .row = e[1],
            .mask = static_cast<std::uint8_t>(e[2] & 0xF),
            .attr = e[3],
            .flat = (e[7] & 1) != 0,
            .fp16 = (e[7] & 8) != 0,
            .pack2 = (e[7] & 0x10) != 0,
            .row2 = e[5],
        });
    }
    const auto size = u32_at(data, kPsPatchSize);
    if (!size) {
        return std::unexpected(size.error());
    }
    if (*size > kPatchSlotDwords) {
        return std::unexpected(malformed("implausible export patch size"));
    }
    for (std::size_t mrt = 0; mrt < kMrts; ++mrt) {
        const std::size_t at = kPsPatches + mrt * kPatchBytes;
        const auto offset = u32_at(data, at);
        if (!offset) {
            return std::unexpected(offset.error());
        }
        if (*offset == std::numeric_limits<std::uint32_t>::max()) {
            continue;
        }
        if ((static_cast<std::uint64_t>(*offset) + *size) * 4 > code_length) {
            return std::unexpected(malformed("export patch outside the code"));
        }
        const std::size_t full = at + 4 + kPatchFull * kPatchSlotDwords * 4;
        ExportPatch patch{.mrt = static_cast<std::uint8_t>(mrt), .offset = *offset, .code = {}};
        for (std::size_t i = 0; i < *size; ++i) {
            const auto word = u32_at(data, full + 4 * i);
            if (!word) {
                return std::unexpected(word.error());
            }
            patch.code.push_back(*word);
        }
        io.patches.push_back(std::move(patch));
    }
    const auto input_addr = u32_at(data, kPsInputAddr);
    if (!input_addr) {
        return std::unexpected(input_addr.error());
    }
    const auto db_shader_control = u32_at(data, kPsDbShaderControl);
    if (!db_shader_control) {
        return std::unexpected(db_shader_control.error());
    }
    io.input_addr = *input_addr;
    io.db_shader_control = *db_shader_control;
    return io;
}

Result<VsIo> vs_io(Bytes data)
{
    const auto inputs = semantics(data, kVsInputs, 5, kVsInputCount);
    if (!inputs) {
        return std::unexpected(inputs.error());
    }
    VsIo io;
    for (std::size_t at = 0; at + 5 <= inputs->size(); at += 5) {
        const Bytes e = inputs->subspan(at, 5);
        io.inputs.push_back(VsInput{
            .usage = static_cast<Usage>(e[0]),
            .row = e[1],
            .mask = static_cast<std::uint8_t>(e[2] & 0xF),
            .vgpr = e[3],
        });
    }
    const auto outputs = semantics(data, kVsOutputs, 6, kVsOutputCount);
    if (!outputs) {
        return std::unexpected(outputs.error());
    }
    for (std::size_t at = 0; at + 6 <= outputs->size(); at += 6) {
        const Bytes e = outputs->subspan(at, 6);
        io.outputs.push_back(VsOutput{
            .usage = static_cast<Usage>(e[0]),
            .row = e[1],
            .param = e[2],
            .fp16 = (e[5] & 1) != 0,
            .pack2 = (e[5] & 2) != 0,
        });
    }
    return io;
}

Result<MeshIo> ms_io(Bytes data, std::uint32_t lds_bytes)
{
    auto vs = vs_io(data);
    if (!vs) {
        return std::unexpected(vs.error());
    }
    const auto info = bytes(data, kMsInfo, 8);
    if (!info) {
        return std::unexpected(malformed("stage data out of bounds"));
    }
    const auto clip_cntl = u32_at(data, kMsClipCntl);
    if (!clip_cntl) {
        return std::unexpected(clip_cntl.error());
    }
    const auto half = [&](std::size_t at) {
        return static_cast<std::uint32_t>((*info)[at]) | (static_cast<std::uint32_t>((*info)[at + 1]) << 8);
    };
    return MeshIo{
        .outputs = std::move(vs->outputs),
        .clip_cntl = *clip_cntl,
        .threads = half(0),
        .max_vertices = half(2),
        .max_primitives = half(4),
        .topology = (*info)[7],
        .lds_bytes = lds_bytes,
    };
}

Result<ComputeIo> cs_io(Bytes data)
{
    ComputeIo io;
    const auto rsrc2 = u32_at(data, kCsRsrc2);
    if (!rsrc2) {
        return std::unexpected(rsrc2.error());
    }
    io.rsrc2 = *rsrc2;
    for (std::size_t i = 0; i < 3; ++i) {
        const auto threads = u32_at(data, kCsThreads + 4 * i);
        if (!threads) {
            return std::unexpected(threads.error());
        }
        io.threads[i] = *threads;
    }
    return io;
}

Result<Bytes> root_table(Bytes payload, Bytes record, std::size_t at, std::size_t count_at)
{
    const auto count = u32_at(record, count_at);
    if (!count) {
        return std::unexpected(count.error());
    }
    const std::uint64_t length = kEntrySize * *count;
    if (length == 0) {
        return Bytes{};
    }
    const auto offset = u64_at(record, at);
    if (!offset) {
        return std::unexpected(offset.error());
    }
    const auto table = bytes(payload, *offset, length);
    if (!table) {
        return std::unexpected(malformed("FSSM root table out of bounds"));
    }
    return *table;
}

Result<std::vector<NativeProgram>> programs(Bytes fssm)
{
    if (!has_magic(fssm, 0, "FSSM")) {
        return std::unexpected(malformed("native blob lacks FSSM magic"));
    }
    const auto version = u32_at(fssm, 4);
    if (!version) {
        return std::unexpected(version.error());
    }
    if (*version != kFssmVersion) {
        return std::unexpected(malformed("unknown FSSM version"));
    }
    const Bytes payload = fssm.subspan(std::min(kFssmHeader, fssm.size()));
    const auto stated = u64_at(fssm, 8);
    if (!stated) {
        return std::unexpected(stated.error());
    }
    if (*stated != payload.size()) {
        return std::unexpected(malformed("FSSM payload size disagrees"));
    }
    const auto slot_count = u64_at(payload, 0);
    if (!slot_count) {
        return std::unexpected(slot_count.error());
    }
    if (*slot_count != kGraphicsSlotCount) {
        return std::unexpected(malformed("unexpected FSSM slot count"));
    }
    std::vector<NativeProgram> out;
    for (const Slot& slot : kSlots) {
        const auto start = u64_at(payload, slot.at);
        if (!start) {
            return std::unexpected(start.error());
        }
        if (*start == kAbsent) {
            continue;
        }
        const auto record = bytes(payload, *start, kRecordSize);
        if (!record) {
            return std::unexpected(malformed("FSSM record out of bounds"));
        }
        const std::uint64_t code_offset = *u64_at(*record, 0);
        const std::uint64_t code_size = *u64_at(*record, 8);
        const auto code = bytes(payload, code_offset, code_size);
        if (!code) {
            return std::unexpected(malformed("native code out of bounds"));
        }
        const std::uint32_t regs = *u32_at(*record, kRecordRegs);
        const auto vgprs = static_cast<std::uint16_t>(regs & 0x3FF);
        if (vgprs > 256) {
            return std::unexpected(malformed("implausible VGPR count"));
        }
        const auto layout_table = root_table(payload, *record, kRecordLayout, kRecordCounts);
        if (!layout_table) {
            return std::unexpected(layout_table.error());
        }
        const auto used_table = root_table(payload, *record, kRecordUsed, kRecordCounts + 4);
        if (!used_table) {
            return std::unexpected(used_table.error());
        }
        NativeProgram program;
        program.stage = slot.stage;
        program.vgprs = vgprs;
        program.sgprs = static_cast<std::uint16_t>((regs >> 20) & 0xFF);
        for (std::size_t at = 0; at + kEntrySize <= layout_table->size(); at += kEntrySize) {
            const Bytes e = layout_table->subspan(at, kEntrySize);
            program.layout.push_back(RootParam{
                .root = static_cast<RootType>(le32(e, 0)),
                .table_offset_dwords = le32(e, 4),
                .sgprs = le32(e, 8),
                .base_register = le32(e, 12),
                .user_data = e[16],
                .slot = e[17],
            });
        }
        for (std::size_t at = 0; at + kEntrySize <= used_table->size(); at += kEntrySize) {
            const Bytes e = used_table->subspan(at, kEntrySize);
            program.used.push_back(UsedResource{
                .kind = static_cast<ResourceType>(e[0]),
                .root = static_cast<RootType>(e[1]),
                .table_offset_dwords = le32(e, 4),
                .lower_bound = le32(e, 8),
                .stride = le32(e, 12),
                .dwords = e[16],
                .user_data = e[17],
                .slot = e[18],
            });
        }
        const Bytes stage_data = payload.subspan(static_cast<std::size_t>(*start + kRecordSize));
        switch (slot.stage) {
        case HwStage::Ps: {
            auto io = ps_io(stage_data, code->size());
            if (!io) {
                return std::unexpected(io.error());
            }
            program.io = std::move(*io);
            break;
        }
        case HwStage::Vs: {
            auto io = vs_io(stage_data);
            if (!io) {
                return std::unexpected(io.error());
            }
            program.io = std::move(*io);
            break;
        }
        case HwStage::Cs: {
            auto io = cs_io(stage_data);
            if (!io) {
                return std::unexpected(io.error());
            }
            program.io = *io;
            break;
        }
        case HwStage::Ms: {
            auto io = ms_io(stage_data, *u32_at(*record, kRecordLds));
            if (!io) {
                return std::unexpected(io.error());
            }
            program.io = std::move(*io);
            break;
        }
        default:
            break;
        }
        program.wave64 = (*u32_at(*record, kRecordFlags) & kFlagWave32) == 0;
        program.code.assign(code->begin(), code->end());
        out.push_back(std::move(program));
    }
    return out;
}

}

const char* hw_stage_name(HwStage stage) noexcept
{
    switch (stage) {
    case HwStage::Ls:
        return "ls";
    case HwStage::Hs:
        return "hs";
    case HwStage::Es:
        return "es";
    case HwStage::Gs:
        return "gs";
    case HwStage::Vs:
        return "vs";
    case HwStage::Ps:
        return "ps";
    case HwStage::Ms:
        return "ms";
    case HwStage::Cs:
        return "cs";
    }
    return "unknown";
}

std::uint8_t ComputeIo::user_sgprs() const noexcept
{
    return static_cast<std::uint8_t>((rsrc2 >> 1) & 0x1F);
}

std::pair<std::array<std::optional<std::uint8_t>, 3>, std::optional<std::uint8_t>> ComputeIo::system_sgprs() const noexcept
{
    std::uint8_t next = user_sgprs();
    const auto take = [&](unsigned bit) -> std::optional<std::uint8_t> {
        if (((rsrc2 >> bit) & 1) == 0) {
            return std::nullopt;
        }
        return next++;
    };
    std::array<std::optional<std::uint8_t>, 3> ids;
    ids[0] = take(7);
    ids[1] = take(8);
    ids[2] = take(9);
    const std::optional<std::uint8_t> info = take(10);
    return {ids, info};
}

std::uint8_t ComputeIo::initialized_sgprs() const noexcept
{
    const auto [ids, info] = system_sgprs();
    std::uint8_t end = user_sgprs();
    for (const auto& sgpr : ids) {
        if (sgpr) {
            end = std::max<std::uint8_t>(end, static_cast<std::uint8_t>(*sgpr + 1));
        }
    }
    if (info) {
        end = std::max<std::uint8_t>(end, static_cast<std::uint8_t>(*info + 1));
    }
    return static_cast<std::uint8_t>(end + (rsrc2 & 1));
}

std::uint8_t ComputeIo::thread_id_vgprs() const noexcept
{
    return static_cast<std::uint8_t>(((rsrc2 >> 11) & 3) + 1);
}

std::uint32_t ComputeIo::lds_bytes() const noexcept
{
    return ((rsrc2 >> 15) & 0x1FF) * 512;
}

std::vector<std::uint8_t> NativeProgram::patched_code() const
{
    std::vector<std::uint8_t> patched = code;
    if (const auto* ps = std::get_if<PsIo>(&io)) {
        for (const ExportPatch& patch : ps->patches) {
            const std::size_t at = static_cast<std::size_t>(patch.offset) * 4;
            for (std::size_t i = 0; i < patch.code.size(); ++i) {
                const std::uint32_t word = patch.code[i];
                for (std::size_t b = 0; b < 4; ++b) {
                    patched[at + 4 * i + b] = static_cast<std::uint8_t>(word >> (8 * b));
                }
            }
        }
    }
    return patched;
}

std::expected<std::vector<std::uint8_t>, ShaderError> unpack(std::span<const std::uint8_t> blob)
{
    if (!has_magic(blob, 0, "XBCH")) {
        return std::unexpected(malformed("native blob lacks XBCH magic"));
    }
    const auto version = u32_at(blob, 4);
    if (!version) {
        return std::unexpected(version.error());
    }
    if (*version != kXbchVersion) {
        return std::unexpected(malformed("unknown XBCH version"));
    }
    const auto packed = u64_at(blob, 8);
    if (!packed) {
        return std::unexpected(packed.error());
    }
    const auto unpacked = u64_at(blob, 16);
    if (!unpacked) {
        return std::unexpected(unpacked.error());
    }
    if (*unpacked > kMaxUnpacked) {
        return std::unexpected(malformed("implausible XBCH unpacked size"));
    }
    const auto stream = bytes(blob, kXbchHeader, *packed);
    if (!stream) {
        return std::unexpected(malformed("XBCH stream out of bounds"));
    }
    std::vector<std::uint8_t> out(static_cast<std::size_t>(*unpacked));
    const auto inflated = formats::zlib_inflate(*stream, out);
    if (!inflated || *inflated != out.size()) {
        return std::unexpected(malformed("XBCH stream does not inflate to its stated size"));
    }
    return out;
}

std::expected<std::vector<NativeProgram>, ShaderError> native_programs(std::span<const std::uint8_t> part)
{
    if (!has_magic(part, kXdatOffset, "XDAT")) {
        return std::vector<NativeProgram>{};
    }
    const auto count = u32_at(part, kXdatOffset + 4);
    if (!count) {
        return std::unexpected(count.error());
    }
    if (*count > kMaxXdatEntries) {
        return std::unexpected(malformed("implausible XDAT entry count"));
    }
    std::vector<NativeProgram> out;
    for (std::size_t i = 0; i < *count; ++i) {
        const std::size_t at = kXdatOffset + 12 + 12 * i;
        const auto kind = u32_at(part, at);
        if (!kind) {
            return std::unexpected(kind.error());
        }
        if (*kind != kKindNative) {
            continue;
        }
        const auto offset = u32_at(part, at + 4);
        if (!offset) {
            return std::unexpected(offset.error());
        }
        const auto size = u32_at(part, at + 8);
        if (!size) {
            return std::unexpected(size.error());
        }
        const auto blob = bytes(part, *offset, *size);
        if (!blob) {
            return std::unexpected(malformed("XDAT entry out of bounds"));
        }
        const auto fssm = unpack(*blob);
        if (!fssm) {
            return std::unexpected(fssm.error());
        }
        auto found = programs(*fssm);
        if (!found) {
            return std::unexpected(found.error());
        }
        for (NativeProgram& program : *found) {
            out.push_back(std::move(program));
        }
    }
    return out;
}

}
