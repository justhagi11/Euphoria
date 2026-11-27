#include <euphoria/shader/gfx10/dxbc.hpp>
#include <euphoria/shader/gfx10/native.hpp>
#include <euphoria/shader/gfx10/selftest.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace euphoria::shader::gfx10::selftest {
namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::size_t kXdatOffset = 0x18;
constexpr std::uint32_t kKindNative = 0x0d;
constexpr std::uint32_t kXbchVersion = 3;
constexpr std::size_t kXbchHeader = 0x18;
constexpr std::uint32_t kFssmVersion = 0x101;
constexpr std::size_t kFssmHeader = 0x10;
constexpr std::uint64_t kGraphicsSlotCount = 6;
constexpr std::uint64_t kAbsent = 0xDEADBEEFDEADBEEFull;
constexpr std::size_t kRecordSize = 0x54;
constexpr std::uint32_t kFlagWave32 = 1u << 5;
constexpr std::size_t kRecordRegs = 0x50;
constexpr std::size_t kRecordLayout = 0x28;
constexpr std::size_t kRecordUsed = 0x30;
constexpr std::size_t kRecordCounts = 0x38;
constexpr std::size_t kEntrySize = 20;
constexpr std::size_t kPsInputs = 0x24;
constexpr std::size_t kPsInputCount = 0x124;
constexpr std::size_t kPsInputAddr = 0x134;
constexpr std::size_t kPsPatchSize = 0x148;
constexpr std::size_t kPsPatches = 0x14c;
constexpr std::size_t kPatchSlotDwords = 10;
constexpr std::size_t kPatchFull = 9;
constexpr std::size_t kPatchBytes = 4 + 10 * kPatchSlotDwords * 4;
constexpr std::size_t kMrts = 8;
constexpr std::size_t kVsOutputs = 0xc8;
constexpr std::size_t kVsOutputCount = 0x188;
constexpr std::size_t kRecordLds = 0x4c;
constexpr std::size_t kMsClipCntl = 0x194;
constexpr std::size_t kMsInfo = 0x448;
constexpr std::size_t kStageData = kPsPatches + kMrts * kPatchBytes;
constexpr std::uint32_t kVgprs = 28;

constexpr std::array<std::uint8_t, 8> kVsCode{1, 2, 3, 4, 5, 6, 7, 8};
constexpr std::array<std::uint8_t, 4> kPsCode{9, 10, 11, 12};

constexpr std::array<std::pair<HwStage, std::size_t>, 8> kSlots{{
    {HwStage::Ls, 0x08},
    {HwStage::Hs, 0x10},
    {HwStage::Es, 0x18},
    {HwStage::Gs, 0x20},
    {HwStage::Vs, 0x28},
    {HwStage::Ps, 0x30},
    {HwStage::Ms, 0x48},
    {HwStage::Cs, 0x60},
}};

std::size_t slot(HwStage stage)
{
    return std::ranges::find(kSlots, stage, &std::pair<HwStage, std::size_t>::first)->second;
}

void put32(Bytes& out, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

void put64(Bytes& out, std::uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

void set32(Bytes& out, std::size_t at, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) {
        out[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

void set64(Bytes& out, std::size_t at, std::uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        out[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
    }
}

void set_bytes(Bytes& out, std::size_t at, std::initializer_list<std::uint8_t> values)
{
    std::ranges::copy(values, out.begin() + static_cast<std::ptrdiff_t>(at));
}

std::uint64_t get64(const Bytes& data, std::size_t at)
{
    return *u64_at(data, at);
}

Bytes empty_stage_data()
{
    Bytes data(kStageData, 0);
    for (std::size_t mrt = 0; mrt < kMrts; ++mrt) {
        set32(data, kPsPatches + mrt * kPatchBytes, std::numeric_limits<std::uint32_t>::max());
    }
    return data;
}

std::size_t stage_data_at(const Bytes& blob, HwStage stage)
{
    return kFssmHeader + static_cast<std::size_t>(get64(blob, kFssmHeader + slot(stage))) + kRecordSize;
}

Bytes fssm(std::initializer_list<std::pair<HwStage, std::span<const std::uint8_t>>> programs)
{
    Bytes payload;
    put64(payload, kGraphicsSlotCount);
    payload.resize(slot(HwStage::Cs) + 8, 0);
    for (const auto& [stage, at] : kSlots) {
        set64(payload, at, kAbsent);
    }
    for (const auto& [stage, code] : programs) {
        const std::size_t record = payload.size();
        set64(payload, slot(stage), record);
        put64(payload, record + kRecordSize + kStageData);
        put64(payload, code.size());
        put32(payload, 0);
        put32(payload, stage == HwStage::Ps ? kFlagWave32 : 0);
        payload.resize(record + kRecordRegs, 0);
        put32(payload, kVgprs | (31u << 20));
        const Bytes stage_data = empty_stage_data();
        payload.insert(payload.end(), stage_data.begin(), stage_data.end());
        payload.insert(payload.end(), code.begin(), code.end());
    }
    Bytes out{'F', 'S', 'S', 'M'};
    put32(out, kFssmVersion);
    put64(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::uint32_t adler32(std::span<const std::uint8_t> data)
{
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : data) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

Bytes stored_zlib(std::span<const std::uint8_t> plain)
{
    Bytes out{0x78, 0x01};
    std::size_t at = 0;
    do {
        const std::size_t length = std::min<std::size_t>(plain.size() - at, 0xFFFF);
        const bool last = at + length == plain.size();
        out.push_back(last ? 1 : 0);
        out.push_back(static_cast<std::uint8_t>(length));
        out.push_back(static_cast<std::uint8_t>(length >> 8));
        out.push_back(static_cast<std::uint8_t>(~length));
        out.push_back(static_cast<std::uint8_t>(~length >> 8));
        out.insert(out.end(), plain.begin() + static_cast<std::ptrdiff_t>(at),
                   plain.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
    } while (at < plain.size());
    const std::uint32_t checksum = adler32(plain);
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(checksum >> shift));
    }
    return out;
}

Bytes xbch(std::span<const std::uint8_t> unpacked)
{
    const Bytes stream = stored_zlib(unpacked);
    Bytes out{'X', 'B', 'C', 'H'};
    put32(out, kXbchVersion);
    put64(out, stream.size());
    put64(out, unpacked.size());
    out.insert(out.end(), stream.begin(), stream.end());
    return out;
}

Bytes part(std::initializer_list<std::pair<std::uint32_t, std::span<const std::uint8_t>>> blobs)
{
    Bytes out(kXdatOffset, 0);
    out.insert(out.end(), {'X', 'D', 'A', 'T'});
    put32(out, static_cast<std::uint32_t>(blobs.size()));
    put32(out, 0x24);
    std::size_t offset = out.size() + 12 * blobs.size();
    for (const auto& [kind, blob] : blobs) {
        put32(out, kind);
        put32(out, static_cast<std::uint32_t>(offset));
        put32(out, static_cast<std::uint32_t>(blob.size()));
        offset += blob.size();
    }
    for (const auto& [kind, blob] : blobs) {
        out.insert(out.end(), blob.begin(), blob.end());
    }
    return out;
}

const std::array<std::uint8_t, 4> kXbop{'x', 'b', 'o', 'p'};

void finds_graphics_programs(Context& check)
{
    const Bytes native = xbch(fssm({{HwStage::Vs, kVsCode}, {HwStage::Ps, kPsCode}}));
    const Bytes data = part({{0x0a, kXbop}, {kKindNative, native}});
    const auto found = native_programs(data);
    check.expect(found.has_value(), "graphics programs parse");
    if (!found) {
        return;
    }
    const NativeProgram vs{
        .stage = HwStage::Vs,
        .wave64 = true,
        .vgprs = 28,
        .layout = {},
        .used = {},
        .io = VsIo{},
        .code = Bytes(kVsCode.begin(), kVsCode.end()),
        .sgprs = 31,
    };
    const NativeProgram ps{
        .stage = HwStage::Ps,
        .wave64 = false,
        .vgprs = 28,
        .layout = {},
        .used = {},
        .io = PsIo{},
        .code = Bytes(kPsCode.begin(), kPsCode.end()),
        .sgprs = 31,
    };
    check.expect(found->size() == 2 && (*found)[0] == vs && (*found)[1] == ps, "vertex then pixel program");
}

void reads_root_tables(Context& check)
{
    Bytes blob = fssm({{HwStage::Ps, kPsCode}});
    const std::size_t record = kFssmHeader + static_cast<std::size_t>(get64(blob, kFssmHeader + slot(HwStage::Ps)));
    const std::uint64_t tables = blob.size() - kFssmHeader;
    for (const std::uint32_t v : {3u, 2u, 1u, 5u}) {
        put32(blob, v);
    }
    blob.insert(blob.end(), {9, 1, 0, 0, 0, 3, 0, 0});
    for (const std::uint32_t v : {2u, 5u, 4u}) {
        put32(blob, v);
    }
    blob.insert(blob.end(), {8, 9, 1, 0});
    set64(blob, 8, blob.size() - kFssmHeader);
    set64(blob, record + kRecordLayout, tables);
    set64(blob, record + kRecordUsed, tables + kEntrySize);
    set_bytes(blob, record + kRecordCounts, {1, 0, 0, 0, 1, 0, 0, 0});

    const auto found = native_programs(part({{kKindNative, xbch(blob)}}));
    check.expect(found.has_value() && found->size() == 1, "one pixel program");
    if (!found || found->empty()) {
        return;
    }
    const NativeProgram& p = (*found)[0];
    const RootParam param{
        .root = RootType::Table,
        .table_offset_dwords = 2,
        .sgprs = 1,
        .base_register = 5,
        .user_data = 9,
        .slot = 1,
    };
    check.expect(p.layout.size() == 1 && p.layout[0] == param, "root layout entry");
    const UsedResource used{
        .kind = ResourceType::Srv,
        .root = RootType::Table,
        .table_offset_dwords = 2,
        .lower_bound = 5,
        .stride = 4,
        .dwords = 8,
        .user_data = 9,
        .slot = 1,
    };
    check.expect(p.used.size() == 1 && p.used[0] == used, "used resource entry");

    blob[record + kRecordCounts] = 9;
    check.expect(!native_programs(part({{kKindNative, xbch(blob)}})), "root table out of bounds is rejected");
}

void reads_pixel_io_and_applies_the_full_export_patch(Context& check)
{
    Bytes code;
    for (int i = 0; i < 4; ++i) {
        put32(code, 0xBF800000u);
    }
    Bytes blob = fssm({{HwStage::Ps, code}});
    const std::size_t e = stage_data_at(blob, HwStage::Ps);
    set_bytes(blob, e + kPsInputs, {6, 1, 3, 0, 0, 0, 1, 1});
    set32(blob, e + kPsInputCount, 1);
    set32(blob, e + kPsInputAddr, 2);
    set32(blob, e + kPsPatchSize, 2);
    const std::size_t patch = e + kPsPatches + kPatchBytes;
    set32(blob, patch, 1);
    const std::size_t full = patch + 4 + kPatchFull * kPatchSlotDwords * 4;
    set_bytes(blob, full, {0x1f, 0x18, 0, 0xf8, 0, 1, 2, 3});

    const auto found = native_programs(part({{kKindNative, xbch(blob)}}));
    check.expect(found.has_value() && found->size() == 1, "one pixel program");
    if (!found || found->empty()) {
        return;
    }
    const NativeProgram& p = (*found)[0];
    const auto* io = std::get_if<PsIo>(&p.io);
    check.expect(io != nullptr, "pixel stage io");
    if (io == nullptr) {
        return;
    }
    const PsInput input{
        .usage = Usage::Generic,
        .row = 1,
        .mask = 3,
        .attr = 0,
        .flat = true,
        .fp16 = false,
        .pack2 = false,
        .row2 = 0,
    };
    check.expect(io->inputs.size() == 1 && io->inputs[0] == input && io->input_addr == 2, "pixel input and input address");
    check.expect(io->patches.size() == 1 && io->patches[0].mrt == 1 && io->patches[0].offset == 1, "MRT1 patch at dword 1");
    const Bytes patched = p.patched_code();
    const std::array<std::uint8_t, 8> exported{0x1f, 0x18, 0, 0xf8, 0, 1, 2, 3};
    check.expect(patched.size() == p.code.size() && std::equal(patched.begin(), patched.begin() + 4, p.code.begin())
               && std::equal(patched.begin() + 4, patched.begin() + 12, exported.begin())
               && std::equal(patched.begin() + 12, patched.end(), p.code.begin() + 12),
           "patched code replaces only the export");

    set32(blob, patch, 3);
    check.expect(!native_programs(part({{kKindNative, xbch(blob)}})), "patch outside the code is rejected");
}

void finds_mesh_and_compute_programs(Context& check)
{
    for (const HwStage stage : {HwStage::Ms, HwStage::Cs}) {
        const auto found = native_programs(part({{kKindNative, xbch(fssm({{stage, kVsCode}}))}}));
        check.expect(found.has_value() && found->size() == 1 && (*found)[0].stage == stage, hw_stage_name(stage));
    }
}

void reads_mesh_outputs_and_limits(Context& check)
{
    Bytes blob = fssm({{HwStage::Ms, kVsCode}});
    const std::size_t e = stage_data_at(blob, HwStage::Ms);
    set_bytes(blob, e + kVsOutputs, {6, 1, 0, 0, 0, 0, 6, 0, 0x82, 0, 0, 0});
    set32(blob, e + kVsOutputCount, 2);
    set32(blob, e + kMsClipCntl, 0x0040000F);
    set_bytes(blob, e + kMsInfo, {0x80, 0, 0x80, 0, 0x40, 0, 13, 2});
    set32(blob, e - kRecordSize + kRecordLds, 16);
    const auto found = native_programs(part({{kKindNative, xbch(blob)}}));
    check.expect(found.has_value() && found->size() == 1, "one mesh program");
    if (!found || found->empty()) {
        return;
    }
    const auto out = [](std::uint8_t row, std::uint8_t param) {
        return VsOutput{.usage = Usage::Generic, .row = row, .param = param, .fp16 = false, .pack2 = false};
    };
    const MeshIo mesh{
        .outputs = {out(1, 0), out(0, 0x82)},
        .clip_cntl = 0x0040000F,
        .threads = 128,
        .max_vertices = 128,
        .max_primitives = 64,
        .topology = 2,
        .lds_bytes = 16,
    };
    check.expect((*found)[0].io == StageIo{mesh}, "mesh outputs and limits");
}

void compute_dispatch_settings(Context& check)
{
    using Ids = std::array<std::optional<std::uint8_t>, 3>;
    const auto s = [](std::uint8_t sgpr) { return std::optional<std::uint8_t>{sgpr}; };
    const ComputeIo a{.rsrc2 = 0x1098A, .threads = {16, 16, 1}};
    check.expect(a.user_sgprs() == 5, "5 user SGPRs");
    check.expect(a.system_sgprs() == std::pair{Ids{s(5), s(6), std::nullopt}, std::optional<std::uint8_t>{}}, "group ids x and y");
    check.expect(a.thread_id_vgprs() == 2 && a.lds_bytes() == 1024, "v0-v1 and 1 KiB LDS");
    const ComputeIo b{.rsrc2 = 0x138C, .threads = {8, 8, 1}};
    check.expect(b.system_sgprs() == std::pair{Ids{s(6), s(7), s(8)}, std::optional<std::uint8_t>{}}, "all group ids after 6 user SGPRs");
    check.expect(b.thread_id_vgprs() == 3 && b.lds_bytes() == 0, "v0-v2 and no LDS");
}

void initialized_sgprs_covers_user_system_and_scratch(Context& check)
{
    check.expect(ComputeIo{.rsrc2 = 0x1098A, .threads = {16, 16, 1}}.initialized_sgprs() == 7, "user and group ids");
    check.expect(ComputeIo{.rsrc2 = 0x78C, .threads = {8, 8, 1}}.initialized_sgprs() == 10, "with the info SGPR");
    check.expect(ComputeIo{.rsrc2 = 0x78D, .threads = {8, 8, 1}}.initialized_sgprs() == 11, "with the scratch wave offset");
}

void reads_every_native_blob(Context& check)
{
    const Bytes es = xbch(fssm({{HwStage::Es, kVsCode}}));
    const Bytes vs = xbch(fssm({{HwStage::Vs, kVsCode}}));
    const std::array<std::uint8_t, 16> hash{};
    const auto found = native_programs(part({{0x12, hash}, {kKindNative, es}, {kKindNative, vs}}));
    check.expect(found.has_value() && found->size() == 2 && (*found)[0].stage == HwStage::Es && (*found)[1].stage == HwStage::Vs,
           "both native blobs are read in order");
}

void pc_layout_has_none(Context& check)
{
    const std::array<std::uint8_t, 64> zeros{};
    const auto pc = native_programs(zeros);
    check.expect(pc.has_value() && pc->empty(), "no XDAT means no programs");
    const auto options = native_programs(part({{0x0a, kXbop}}));
    check.expect(options.has_value() && options->empty(), "XDAT without native blobs means no programs");
}

void rejects_corrupt_blobs(Context& check)
{
    const Bytes good = xbch(fssm({{HwStage::Ls, kVsCode}}));
    const auto corrupt = [&](std::size_t at, std::uint8_t byte) {
        Bytes blob = good;
        blob[at] = byte;
        return native_programs(part({{kKindNative, blob}}));
    };
    check.expect(!corrupt(4, 9), "unknown version");
    check.expect(!corrupt(16, 0xFF), "stated size");
    check.expect(!corrupt(kXbchHeader + 2, 0xFF), "zlib stream");
    check.expect(!native_programs(part({{kKindNative, std::span<const std::uint8_t>{good}.first(20)}})), "truncated header");

    Bytes bad_record = fssm({{HwStage::Ls, kVsCode}});
    set64(bad_record, kFssmHeader + slot(HwStage::Ls), std::numeric_limits<std::uint64_t>::max());
    check.expect(!native_programs(part({{kKindNative, xbch(bad_record)}})), "record out of bounds");
}

void rejects_truncated_oversized_and_bad_checksum_xbch_streams(Context& check)
{
    const Bytes plain = fssm({{HwStage::Vs, kVsCode}});
    const Bytes good = xbch(plain);
    const auto unpacked = unpack(good);
    check.expect(unpacked.has_value() && *unpacked == plain, "good stream unpacks");

    Bytes truncated(good.begin(), good.end() - 1);
    set64(truncated, 8, truncated.size() - kXbchHeader);
    check.expect(!unpack(truncated), "truncated stream");

    Bytes oversized = good;
    set64(oversized, 16, plain.size() - 1);
    check.expect(!unpack(oversized), "stated size smaller than the output");

    Bytes underfilled = good;
    set64(underfilled, 16, plain.size() + 1);
    check.expect(!unpack(underfilled), "stated size larger than the output");

    Bytes bad_adler = good;
    bad_adler.back() ^= 1;
    check.expect(!unpack(bad_adler), "corrupt Adler-32");
}

void maps_user_sgprs(Context& check)
{
    check.expect(user_sgpr(HwStage::Ps, 3) == 3 && user_sgpr(HwStage::Cs, 0) == 0 && user_sgpr(HwStage::Vs, 1) == 1,
           "hardware stages with user data first");
    check.expect(user_sgpr(HwStage::Ms, 2) == 10 && user_sgpr(HwStage::Gs, 0) == 8 && user_sgpr(HwStage::Ls, 1) == 9,
           "merged stages after 8 system SGPRs");
}

}

void register_native_tests()
{
    register_case("native", "finds_graphics_programs", finds_graphics_programs);
    register_case("native", "reads_root_tables", reads_root_tables);
    register_case("native", "reads_pixel_io_and_applies_the_full_export_patch", reads_pixel_io_and_applies_the_full_export_patch);
    register_case("native", "finds_mesh_and_compute_programs", finds_mesh_and_compute_programs);
    register_case("native", "reads_mesh_outputs_and_limits", reads_mesh_outputs_and_limits);
    register_case("native", "compute_dispatch_settings", compute_dispatch_settings);
    register_case("native", "initialized_sgprs_covers_user_system_and_scratch", initialized_sgprs_covers_user_system_and_scratch);
    register_case("native", "reads_every_native_blob", reads_every_native_blob);
    register_case("native", "pc_layout_has_none", pc_layout_has_none);
    register_case("native", "rejects_corrupt_blobs", rejects_corrupt_blobs);
    register_case("native", "rejects_truncated_oversized_and_bad_checksum_xbch_streams",
                 rejects_truncated_oversized_and_bad_checksum_xbch_streams);
    register_case("native", "maps_user_sgprs", maps_user_sgprs);
}

}
