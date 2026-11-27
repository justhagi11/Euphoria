#pragma once

#include <euphoria/shader/gfx10/error.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <utility>
#include <variant>
#include <vector>

namespace euphoria::shader::gfx10 {

enum class HwStage : std::uint8_t {
    Ls,
    Hs,
    Es,
    Gs,
    Vs,
    Ps,
    Ms,
    Cs,
};

constexpr std::uint8_t user_sgpr(HwStage stage, std::uint8_t index) noexcept
{
    switch (stage) {
    case HwStage::Ls:
    case HwStage::Hs:
    case HwStage::Es:
    case HwStage::Gs:
    case HwStage::Ms:
        return static_cast<std::uint8_t>(index + 8);
    case HwStage::Vs:
    case HwStage::Ps:
    case HwStage::Cs:
        break;
    }
    return index;
}

const char* hw_stage_name(HwStage stage) noexcept;

enum class Usage : std::uint8_t {
    Generic = 6,
    ClipDistance = 7,
};

struct PsInput {
    Usage usage{Usage::Generic};
    std::uint8_t row{};
    std::uint8_t mask{};
    std::uint8_t attr{};
    bool flat{false};
    bool fp16{false};
    bool pack2{false};
    std::uint8_t row2{};

    friend constexpr bool operator==(const PsInput&, const PsInput&) noexcept = default;
};

struct VsInput {
    Usage usage{Usage::Generic};
    std::uint8_t row{};
    std::uint8_t mask{};
    std::uint8_t vgpr{};

    friend constexpr bool operator==(const VsInput&, const VsInput&) noexcept = default;
};

struct VsOutput {
    Usage usage{Usage::Generic};
    std::uint8_t row{};
    std::uint8_t param{};
    bool fp16{false};
    bool pack2{false};

    friend constexpr bool operator==(const VsOutput&, const VsOutput&) noexcept = default;
};

struct ExportPatch {
    std::uint8_t mrt{};
    std::uint32_t offset{};
    std::vector<std::uint32_t> code;

    friend bool operator==(const ExportPatch&, const ExportPatch&) = default;
};

struct VsIo {
    std::vector<VsInput> inputs;
    std::vector<VsOutput> outputs;

    friend bool operator==(const VsIo&, const VsIo&) = default;
};

struct PsIo {
    std::vector<PsInput> inputs;
    std::uint32_t input_addr{};
    std::uint32_t db_shader_control{};
    std::vector<ExportPatch> patches;

    friend bool operator==(const PsIo&, const PsIo&) = default;
};

struct ComputeIo {
    std::uint32_t rsrc2{};
    std::array<std::uint32_t, 3> threads{};

    std::uint8_t user_sgprs() const noexcept;
    std::pair<std::array<std::optional<std::uint8_t>, 3>, std::optional<std::uint8_t>> system_sgprs() const noexcept;
    std::uint8_t initialized_sgprs() const noexcept;
    std::uint8_t thread_id_vgprs() const noexcept;
    std::uint32_t lds_bytes() const noexcept;

    friend constexpr bool operator==(const ComputeIo&, const ComputeIo&) noexcept = default;
};

struct MeshIo {
    std::vector<VsOutput> outputs;
    std::uint32_t clip_cntl{};
    std::uint32_t threads{};
    std::uint32_t max_vertices{};
    std::uint32_t max_primitives{};
    std::uint8_t topology{};
    std::uint32_t lds_bytes{};

    friend bool operator==(const MeshIo&, const MeshIo&) = default;
};

using StageIo = std::variant<std::monostate, VsIo, PsIo, ComputeIo, MeshIo>;

enum class RootType : std::uint32_t {
    Constants = 0,
    Descriptor = 1,
    Table = 3,
    ImmediateSampler = 4,
    ResourceHeap = 5,
    SamplerHeap = 6,
};

struct RootParam {
    RootType root{RootType::Constants};
    std::uint32_t table_offset_dwords{};
    std::uint32_t sgprs{};
    std::uint32_t base_register{};
    std::uint8_t user_data{};
    std::uint8_t slot{};

    friend constexpr bool operator==(const RootParam&, const RootParam&) noexcept = default;
};

enum class ResourceType : std::uint8_t {
    Srv = 0,
    Sampler = 1,
    Cbv = 2,
    Uav = 3,
};

struct UsedResource {
    ResourceType kind{ResourceType::Srv};
    RootType root{RootType::Constants};
    std::uint32_t table_offset_dwords{};
    std::uint32_t lower_bound{};
    std::uint32_t stride{};
    std::uint8_t dwords{};
    std::uint8_t user_data{};
    std::uint8_t slot{};

    friend constexpr bool operator==(const UsedResource&, const UsedResource&) noexcept = default;
};

struct NativeProgram {
    HwStage stage{HwStage::Cs};
    bool wave64{true};
    std::uint16_t vgprs{};
    std::vector<RootParam> layout;
    std::vector<UsedResource> used;
    StageIo io;
    std::vector<std::uint8_t> code;
    std::uint16_t sgprs{};

    std::vector<std::uint8_t> patched_code() const;

    friend bool operator==(const NativeProgram&, const NativeProgram&) = default;
};

std::expected<std::vector<NativeProgram>, ShaderError> native_programs(std::span<const std::uint8_t> part);

std::expected<std::vector<std::uint8_t>, ShaderError> unpack(std::span<const std::uint8_t> blob);

}

namespace euphoria::shader::gfx10::native {

using HwStage = ::euphoria::shader::gfx10::HwStage;
using Usage = ::euphoria::shader::gfx10::Usage;
using PsInput = ::euphoria::shader::gfx10::PsInput;
using VsInput = ::euphoria::shader::gfx10::VsInput;
using VsOutput = ::euphoria::shader::gfx10::VsOutput;
using VsIo = ::euphoria::shader::gfx10::VsIo;
using PsIo = ::euphoria::shader::gfx10::PsIo;
using ComputeIo = ::euphoria::shader::gfx10::ComputeIo;
using MeshIo = ::euphoria::shader::gfx10::MeshIo;
using RootParam = ::euphoria::shader::gfx10::RootParam;
using NativeProgram = ::euphoria::shader::gfx10::NativeProgram;

}
