#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <d3d12.h>

namespace euphoria::gpu {

struct Bytecode {
    const std::uint8_t* code{nullptr};
    std::size_t size{0};
};

struct GraphicsHead {
    void* root{nullptr};
    Bytecode stages[5]{};
};

struct ComputeHead {
    void* root{nullptr};
    Bytecode stage{};
};

struct StreamDesc {
    std::size_t size{0};
    const void* stream{nullptr};
};

enum class PipelineStage : std::uint8_t {
    Vertex,
    Pixel,
    Domain,
    Hull,
    Geometry,
    Compute,
    Amplification,
    Mesh,
    Unknown,
};

enum class RootSignatureVersion : std::uint32_t {
    Version10 = 0,
    Version11 = 1,
};

struct RootSignatureDesc {
    std::uint32_t parameter_count{0};
    const void* parameters{nullptr};
    std::uint32_t sampler_count{0};
    const void* samplers{nullptr};
    std::uint32_t flags{0};
};

struct VersionedRootSignatureDesc {
    std::uint32_t version{0};
    RootSignatureDesc desc{};
};

struct Pipeline {
    ID3D12PipelineState* state{nullptr};
    ID3D12RootSignature* root{nullptr};
    std::uint64_t key{0};
};

inline constexpr std::uint32_t kConsoleRootSignatureFlags = 0xFF000000u;

static_assert(sizeof(Bytecode) == 16);
static_assert(offsetof(Bytecode, code) == 0);
static_assert(offsetof(Bytecode, size) == 8);
static_assert(offsetof(GraphicsHead, root) == 0);
static_assert(offsetof(GraphicsHead, stages) == 8);
static_assert(sizeof(GraphicsHead) == 88);
static_assert(offsetof(ComputeHead, root) == 0);
static_assert(offsetof(ComputeHead, stage) == 8);
static_assert(sizeof(ComputeHead) == 24);
static_assert(sizeof(StreamDesc) == 16);
static_assert(offsetof(StreamDesc, size) == 0);
static_assert(offsetof(StreamDesc, stream) == 8);
static_assert(sizeof(RootSignatureDesc) == 40);
static_assert(offsetof(RootSignatureDesc, parameter_count) == 0);
static_assert(offsetof(RootSignatureDesc, parameters) == 8);
static_assert(offsetof(RootSignatureDesc, sampler_count) == 16);
static_assert(offsetof(RootSignatureDesc, samplers) == 24);
static_assert(offsetof(RootSignatureDesc, flags) == 32);
static_assert(sizeof(VersionedRootSignatureDesc) == 48);
static_assert(offsetof(VersionedRootSignatureDesc, version) == 0);
static_assert(offsetof(VersionedRootSignatureDesc, desc) == 8);

HRESULT serialize_root_signature(const RootSignatureDesc* desc, RootSignatureVersion version,
                                 ID3DBlob** blob, ID3DBlob** error);
HRESULT serialize_versioned_root_signature(const VersionedRootSignatureDesc* desc,
                                           ID3DBlob** blob, ID3DBlob** error);
HRESULT create_root_signature(ID3D12Device* host, const RootSignatureDesc* desc,
                              RootSignatureVersion version, ID3D12RootSignature** out);

HRESULT create_graphics_pipeline(void* owner, ID3D12Device* host, const GraphicsHead* head,
                                 const D3D12_GRAPHICS_PIPELINE_STATE_DESC* console_state,
                                 const RootSignatureDesc* console_root,
                                 Pipeline* out,
                                 RootSignatureVersion root_version = RootSignatureVersion::Version10);

HRESULT create_compute_pipeline(void* owner, ID3D12Device* host, const ComputeHead* head,
                                const RootSignatureDesc* console_root,
                                Pipeline* out,
                                RootSignatureVersion root_version = RootSignatureVersion::Version10);

HRESULT create_compute_pipeline_x(void* owner, ID3D12Device* host, const ComputeHead* head,
                                  std::uint32_t extension_count, const void* extensions,
                                  const RootSignatureDesc* console_root,
                                  Pipeline* out,
                                  RootSignatureVersion root_version = RootSignatureVersion::Version10);

HRESULT create_pipeline_from_stream(void* owner, ID3D12Device* host, const StreamDesc* desc,
                                    Pipeline* out);

void release_pipeline(Pipeline& pipeline) noexcept;
void clear_pipeline_cache();
std::uint64_t pipeline_cache_hits() noexcept;
std::uint64_t pipeline_cache_misses() noexcept;
std::uint64_t shader_translation_hits() noexcept;
std::uint64_t shader_translation_misses() noexcept;
std::size_t translated_shader_count() noexcept;
std::size_t cached_pipeline_count() noexcept;
const char* stage_name(PipelineStage stage) noexcept;
std::span<const std::uint8_t> translate_shader(const Bytecode& bytecode);

}
