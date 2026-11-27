#include <windows.h>

#include <euphoria/gpu/pipeline.hpp>

#include <euphoria/log/log.hpp>
#include <euphoria/shader/dxc.hpp>
#include <euphoria/shader/lift_hlsl.hpp>
#include <euphoria/shader/translator.hpp>
#include <euphoria/shader/xbox.hpp>

#include <openscarlett/gpu/rdna2_disasm.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace euphoria::gpu {
namespace {

using Hash = std::uint64_t;

constexpr Hash kHashOffset = 14695981039346656037ull;
constexpr Hash kHashPrime = 1099511628211ull;

constexpr std::size_t kGraphicsStages = 5;
constexpr std::size_t kMaxRenderTargets = 8;
constexpr std::size_t kConsoleRootSignatureBytes = 36;
constexpr std::size_t kSubobjectUnknown = 16;
constexpr std::size_t kSubobjectRootSignature = 40;
constexpr std::size_t kSubobjectStage = 24;
constexpr std::size_t kSubobjectBlend = 336;
constexpr std::size_t kSubobjectRasterizer = 48;
constexpr std::size_t kSubobjectDepth = 56;
constexpr std::size_t kSubobjectDepth1 = 64;
constexpr std::size_t kSubobjectOpaque = 32;
constexpr std::size_t kSubobjectScalar = 8;
constexpr std::size_t kSubobjectInputLayout = 24;
constexpr std::size_t kSubobjectRtFormats = 40;
constexpr std::size_t kSubobjectSamples = 16;

constexpr std::size_t align8(std::size_t value)
{
    return (value + 7u) & ~static_cast<std::size_t>(7u);
}

static_assert(sizeof(D3D12_RENDER_TARGET_BLEND_DESC) == 40);
static_assert(offsetof(D3D12_RENDER_TARGET_BLEND_DESC, RenderTargetWriteMask) == 36);
static_assert(sizeof(D3D12_BLEND_DESC) == 328);
static_assert(sizeof(D3D12_RASTERIZER_DESC) == 44);
static_assert(sizeof(D3D12_DEPTH_STENCIL_DESC) == 52);
static_assert(sizeof(D3D12_DEPTH_STENCIL_DESC1) == 56);
static_assert(sizeof(D3D12_INPUT_LAYOUT_DESC) == 16);
static_assert(sizeof(D3D12_RT_FORMAT_ARRAY) == 36);
static_assert(sizeof(DXGI_SAMPLE_DESC) == 8);
static_assert(kSubobjectUnknown == 16);
static_assert(kSubobjectRootSignature == 40);
static_assert(kSubobjectBlend == 336);
static_assert(kSubobjectRasterizer == 48);
static_assert(kSubobjectDepth == 56);
static_assert(kSubobjectStage == 24);
static_assert(kSubobjectDepth1 == 64);
static_assert(kSubobjectOpaque == 32);
static_assert(kSubobjectScalar == 8);
static_assert(kSubobjectBlend == align8(4 + sizeof(D3D12_BLEND_DESC)));
static_assert(kSubobjectRasterizer == align8(4 + sizeof(D3D12_RASTERIZER_DESC)));
static_assert(kSubobjectDepth == align8(4 + sizeof(D3D12_DEPTH_STENCIL_DESC)));
static_assert(kSubobjectDepth1 == align8(4 + sizeof(D3D12_DEPTH_STENCIL_DESC1)));
static_assert(kSubobjectRtFormats == align8(4 + sizeof(D3D12_RT_FORMAT_ARRAY)));
static_assert(kSubobjectInputLayout == align8(8 + sizeof(D3D12_INPUT_LAYOUT_DESC)));
static_assert(kSubobjectStage == align8(8 + sizeof(Bytecode)));
static_assert(kSubobjectSamples == align8(4 + sizeof(DXGI_SAMPLE_DESC)));
static_assert(kSubobjectRootSignature == align8(4 + kConsoleRootSignatureBytes));
static_assert(kSubobjectScalar == align8(4 + sizeof(std::uint32_t)));

const char* const kStageNames[kGraphicsStages]{"vertex", "pixel", "domain", "hull", "geometry"};

std::string hex64(Hash value)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string text(16, '0');
    for (std::size_t index = 16; index > 0; --index) {
        text[index - 1] = digits[value & 0xFu];
        value >>= 4;
    }
    return text;
}

std::string hex32(std::uint32_t value)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string text(8, '0');
    for (std::size_t index = 8; index > 0; --index) {
        text[index - 1] = digits[value & 0xFu];
        value >>= 4;
    }
    return text;
}

std::string hr_text(HRESULT result)
{
    return hex32(static_cast<std::uint32_t>(result));
}

std::string pointer_text(const void* pointer)
{
    return hex64(static_cast<Hash>(reinterpret_cast<std::uintptr_t>(pointer)));
}

std::string trim_text(std::string text, std::size_t limit = 240)
{
    if (text.size() > limit) {
        text.resize(limit);
    }
    return text;
}

void mix_byte(Hash& hash, std::uint8_t value)
{
    hash ^= value;
    hash *= kHashPrime;
}

void mix_u32(Hash& hash, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8) {
        mix_byte(hash, static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void mix_u64(Hash& hash, std::uint64_t value)
{
    for (int shift = 0; shift < 64; shift += 8) {
        mix_byte(hash, static_cast<std::uint8_t>((value >> shift) & 0xFFu));
    }
}

void mix_bytes(Hash& hash, const void* data, std::size_t size)
{
    mix_u64(hash, static_cast<std::uint64_t>(size));
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t index = 0; index < size; ++index) {
        mix_byte(hash, bytes[index]);
    }
}

void mix_text(Hash& hash, std::string_view text)
{
    mix_bytes(hash, text.data(), text.size());
}

Hash hash_content(std::span<const std::uint8_t> bytes)
{
    Hash hash = kHashOffset;
    mix_bytes(hash, bytes.data(), bytes.size());
    return hash;
}

std::string blob_text(ID3DBlob* blob)
{
    if (blob == nullptr) {
        return {};
    }
    const void* data = blob->GetBufferPointer();
    const SIZE_T size = blob->GetBufferSize();
    if (data == nullptr || size == 0) {
        return {};
    }
    return std::string(static_cast<const char*>(data), size);
}

HMODULE host_module()
{
    static const HMODULE module = [] {
        HMODULE loaded = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (loaded == nullptr) {
            loaded = GetModuleHandleW(L"d3d12.dll");
        }
        return loaded;
    }();
    return module;
}

template <typename Function>
Function host_export(const char* name)
{
    const HMODULE module = host_module();
    if (module == nullptr) {
        return nullptr;
    }
    const FARPROC address = GetProcAddress(module, name);
    if (address == nullptr) {
        return nullptr;
    }
    Function function = nullptr;
    static_assert(sizeof(function) == sizeof(address));
    std::memcpy(&function, &address, sizeof(function));
    return function;
}

using Serialize10 = HRESULT(WINAPI*)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION,
                                      ID3DBlob**, ID3DBlob**);
using SerializeVersioned = HRESULT(WINAPI*)(const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**,
                                            ID3DBlob**);

struct Translation {
    bool ok{};
    std::size_t source_size{};
    std::vector<std::uint8_t> dxil;
    std::string error;
};

struct PipelineEntry {
    ID3D12PipelineState* state{};
    ID3D12RootSignature* root{};
};

std::mutex g_translation_mutex;
std::unordered_map<Hash, Translation> g_translations;
std::atomic<std::uint64_t> g_translation_hits{0};
std::atomic<std::uint64_t> g_translation_misses{0};

std::mutex g_pipeline_mutex;
std::unordered_map<Hash, PipelineEntry> g_pipelines;
std::atomic<std::uint64_t> g_pipeline_hits{0};
std::atomic<std::uint64_t> g_pipeline_misses{0};

std::filesystem::path dxc_directory()
{
    std::error_code error;
    std::filesystem::path directory = std::filesystem::temp_directory_path(error);
    if (error) {
        return std::filesystem::path("euphoria-dxc");
    }
    directory /= "euphoria-dxc";
    return directory;
}

Translation translate_container(std::span<const std::uint8_t> bytes)
{
    Translation result;
    result.source_size = bytes.size();

    const shader::LiftResult lifted = shader::lift(bytes);
    if (lifted.ok && !lifted.dxil.empty()) {
        result.ok = true;
        result.dxil = lifted.dxil;
        return result;
    }

    std::string reason = lifted.error.empty() ? std::string("lift failed") : lifted.error;
    const std::optional<shader::XboxShader> native = shader::extract_xbox_shader(bytes);
    if (!native) {
        result.error = reason;
        return result;
    }

    std::vector<openscarlett::Rdna2Instruction> instructions;
    const openscarlett::Status status =
        openscarlett::DisassembleRdna2(native->code, instructions);
    if (!status.ok()) {
        result.error = reason + "; disassembly: " + status.message;
        return result;
    }

    const shader::HlslLift hlsl = shader::lift_to_hlsl(native->stage, instructions);
    if (!hlsl.ok || hlsl.hlsl.empty()) {
        result.error = reason + "; hlsl: " + (hlsl.error.empty() ? "empty output" : hlsl.error);
        return result;
    }

    const shader::DxcResult compiled = shader::compile_hlsl(hlsl.hlsl, hlsl.stage, dxc_directory());
    if (!compiled.ok || compiled.dxil.empty()) {
        result.error = reason + "; dxc: "
            + (compiled.diagnostics.empty() ? std::string("no output") : compiled.diagnostics);
        return result;
    }

    result.ok = true;
    result.dxil = compiled.dxil;
    return result;
}

const Translation* translated_for(const Bytecode& code, Hash& key)
{
    const std::span<const std::uint8_t> bytes(code.code, code.size);
    key = hash_content(bytes);

    {
        std::lock_guard<std::mutex> lock(g_translation_mutex);
        const auto entry = g_translations.find(key);
        if (entry != g_translations.end()) {
            g_translation_hits.fetch_add(1, std::memory_order_relaxed);
            return &entry->second;
        }
    }

    Translation translation = translate_container(bytes);
    const bool ok = translation.ok;
    const std::size_t size = translation.source_size;
    const std::string error = translation.error;
    Translation* stored = nullptr;

    {
        std::lock_guard<std::mutex> lock(g_translation_mutex);
        auto [entry, inserted] = g_translations.emplace(key, std::move(translation));
        if (!inserted) {
            g_translation_hits.fetch_add(1, std::memory_order_relaxed);
        } else {
            g_translation_misses.fetch_add(1, std::memory_order_relaxed);
        }
        stored = &entry->second;
    }

    if (ok) {
        log::info("shader translate hash=" + hex64(key) + " size=" + std::to_string(size) + " ok");
    } else {
        log::warn("shader translate hash=" + hex64(key) + " size=" + std::to_string(size)
                  + " failed: " + trim_text(error));
    }
    return stored;
}

DXGI_FORMAT host_format(DXGI_FORMAT format)
{
    switch (static_cast<std::uint32_t>(format)) {
    case 118:
        return static_cast<DXGI_FORMAT>(20);
    case 119:
        return static_cast<DXGI_FORMAT>(21);
    case 120:
        return static_cast<DXGI_FORMAT>(22);
    default:
        return format;
    }
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE host_topology(D3D12_PRIMITIVE_TOPOLOGY_TYPE topology)
{
    switch (static_cast<std::uint32_t>(topology)) {
    case 19:
    case 32:
        return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    default:
        return topology;
    }
}

D3D12_GRAPHICS_PIPELINE_STATE_DESC default_graphics_state()
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};

    D3D12_RENDER_TARGET_BLEND_DESC target{};
    target.BlendEnable = FALSE;
    target.LogicOpEnable = FALSE;
    target.SrcBlend = D3D12_BLEND_ONE;
    target.DestBlend = D3D12_BLEND_ZERO;
    target.BlendOp = D3D12_BLEND_OP_ADD;
    target.SrcBlendAlpha = D3D12_BLEND_ONE;
    target.DestBlendAlpha = D3D12_BLEND_ZERO;
    target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    target.LogicOp = D3D12_LOGIC_OP_NOOP;
    target.RenderTargetWriteMask = static_cast<UINT8>(0x0F);
    for (D3D12_RENDER_TARGET_BLEND_DESC& slot : desc.BlendState.RenderTarget) {
        slot = target;
    }

    desc.SampleMask = 0xFFFFFFFFu;

    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.FrontCounterClockwise = FALSE;
    desc.RasterizerState.DepthBias = 0;
    desc.RasterizerState.DepthBiasClamp = 0.0f;
    desc.RasterizerState.SlopeScaledDepthBias = 0.0f;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.MultisampleEnable = FALSE;
    desc.RasterizerState.AntialiasedLineEnable = FALSE;
    desc.RasterizerState.ForcedSampleCount = 0;
    desc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.DepthStencilState.StencilReadMask = static_cast<UINT8>(0xFF);
    desc.DepthStencilState.StencilWriteMask = static_cast<UINT8>(0xFF);
    desc.DepthStencilState.FrontFace.StencilFailOp = D3D12_STENCIL_OP_KEEP;
    desc.DepthStencilState.FrontFace.StencilDepthFailOp = D3D12_STENCIL_OP_KEEP;
    desc.DepthStencilState.FrontFace.StencilPassOp = D3D12_STENCIL_OP_KEEP;
    desc.DepthStencilState.FrontFace.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.DepthStencilState.BackFace = desc.DepthStencilState.FrontFace;

    desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
    desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;

    return desc;
}

struct HostGraphics {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<std::string> names;
};

bool build_graphics_state(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& source, HostGraphics& built)
{
    built = HostGraphics{};
    built.desc = source;
    built.desc.PrimitiveTopologyType = host_topology(source.PrimitiveTopologyType);
    built.desc.NumRenderTargets =
        source.NumRenderTargets > kMaxRenderTargets ? static_cast<UINT>(kMaxRenderTargets)
                                                    : source.NumRenderTargets;
    for (std::size_t index = 0; index < kMaxRenderTargets; ++index) {
        built.desc.RTVFormats[index] = host_format(source.RTVFormats[index]);
    }
    built.desc.DSVFormat = host_format(source.DSVFormat);
    if (built.desc.SampleDesc.Count == 0) {
        built.desc.SampleDesc.Count = 1;
        built.desc.SampleDesc.Quality = 0;
    }

    if (source.CachedPSO.pCachedBlob != nullptr) {
        log::warn("pipeline: cached PSO dropped (" + std::to_string(source.CachedPSO.CachedBlobSizeInBytes)
                  + " bytes)");
    }
    built.desc.CachedPSO.pCachedBlob = nullptr;
    built.desc.CachedPSO.CachedBlobSizeInBytes = 0;

    if (source.StreamOutput.pSODeclaration != nullptr || source.StreamOutput.RasterizedStream != 0) {
        log::warn("pipeline: stream output dropped");
    }
    built.desc.StreamOutput = D3D12_STREAM_OUTPUT_DESC{};

    const UINT count = source.InputLayout.NumElements;
    if (count > 0) {
        if (source.InputLayout.pInputElementDescs == nullptr) {
            log::error("pipeline: input layout has elements but no descriptions");
            return false;
        }
        built.elements.reserve(count);
        built.names.reserve(count);
        for (UINT index = 0; index < count; ++index) {
            D3D12_INPUT_ELEMENT_DESC element = source.InputLayout.pInputElementDescs[index];
            element.Format = host_format(element.Format);
            built.elements.push_back(element);
        }
        for (D3D12_INPUT_ELEMENT_DESC& element : built.elements) {
            if (element.SemanticName == nullptr) {
                log::error("pipeline: input element has no semantic name");
                return false;
            }
            built.names.emplace_back(element.SemanticName);
            element.SemanticName = built.names.back().c_str();
        }
    }
    built.desc.InputLayout.NumElements = static_cast<UINT>(built.elements.size());
    built.desc.InputLayout.pInputElementDescs =
        built.elements.empty() ? nullptr : built.elements.data();
    return true;
}

void mix_blend(Hash& hash, const D3D12_BLEND_DESC& blend)
{
    mix_u32(hash, static_cast<std::uint32_t>(blend.AlphaToCoverageEnable));
    mix_u32(hash, static_cast<std::uint32_t>(blend.IndependentBlendEnable));
    for (const D3D12_RENDER_TARGET_BLEND_DESC& target : blend.RenderTarget) {
        mix_u32(hash, static_cast<std::uint32_t>(target.BlendEnable));
        mix_u32(hash, static_cast<std::uint32_t>(target.LogicOpEnable));
        mix_u32(hash, static_cast<std::uint32_t>(target.SrcBlend));
        mix_u32(hash, static_cast<std::uint32_t>(target.DestBlend));
        mix_u32(hash, static_cast<std::uint32_t>(target.BlendOp));
        mix_u32(hash, static_cast<std::uint32_t>(target.SrcBlendAlpha));
        mix_u32(hash, static_cast<std::uint32_t>(target.DestBlendAlpha));
        mix_u32(hash, static_cast<std::uint32_t>(target.BlendOpAlpha));
        mix_u32(hash, static_cast<std::uint32_t>(target.LogicOp));
        mix_u32(hash, static_cast<std::uint32_t>(target.RenderTargetWriteMask));
    }
}

void mix_rasterizer(Hash& hash, const D3D12_RASTERIZER_DESC& rasterizer)
{
    mix_u32(hash, static_cast<std::uint32_t>(rasterizer.FillMode));
    mix_u32(hash, static_cast<std::uint32_t>(rasterizer.CullMode));
    mix_u32(hash, static_cast<std::uint32_t>(rasterizer.FrontCounterClockwise));
    mix_u32(hash, static_cast<std::uint32_t>(rasterizer.DepthBias));
    mix_u32(hash, std::bit_cast<std::uint32_t>(rasterizer.DepthBiasClamp));
    mix_u32(hash, std::bit_cast<std::uint32_t>(rasterizer.SlopeScaledDepthBias));
    mix_u32(hash, static_cast<std::uint32_t>(rasterizer.DepthClipEnable));
    mix_u32(hash, static_cast<std::uint32_t>(rasterizer.MultisampleEnable));
    mix_u32(hash, static_cast<std::uint32_t>(rasterizer.AntialiasedLineEnable));
    mix_u32(hash, rasterizer.ForcedSampleCount);
    mix_u32(hash, static_cast<std::uint32_t>(rasterizer.ConservativeRaster));
}

void mix_stencil_op(Hash& hash, const D3D12_DEPTH_STENCILOP_DESC& op)
{
    mix_u32(hash, static_cast<std::uint32_t>(op.StencilFailOp));
    mix_u32(hash, static_cast<std::uint32_t>(op.StencilDepthFailOp));
    mix_u32(hash, static_cast<std::uint32_t>(op.StencilPassOp));
    mix_u32(hash, static_cast<std::uint32_t>(op.StencilFunc));
}

void mix_depth(Hash& hash, const D3D12_DEPTH_STENCIL_DESC& depth)
{
    mix_u32(hash, static_cast<std::uint32_t>(depth.DepthEnable));
    mix_u32(hash, static_cast<std::uint32_t>(depth.DepthWriteMask));
    mix_u32(hash, static_cast<std::uint32_t>(depth.DepthFunc));
    mix_u32(hash, static_cast<std::uint32_t>(depth.StencilEnable));
    mix_u32(hash, static_cast<std::uint32_t>(depth.StencilReadMask));
    mix_u32(hash, static_cast<std::uint32_t>(depth.StencilWriteMask));
    mix_stencil_op(hash, depth.FrontFace);
    mix_stencil_op(hash, depth.BackFace);
}

void mix_root(Hash& hash, const std::vector<std::uint8_t>& blob, const void* root)
{
    if (!blob.empty()) {
        mix_bytes(hash, blob.data(), blob.size());
    } else {
        mix_u64(hash, static_cast<Hash>(reinterpret_cast<std::uintptr_t>(root)));
    }
}

Hash graphics_key(const HostGraphics& built,
                  const std::array<const std::vector<std::uint8_t>*, kGraphicsStages>& stages,
                  const std::vector<std::uint8_t>& root_blob, const void* root)
{
    Hash hash = kHashOffset;
    mix_u32(hash, 0x47505053u);
    for (std::size_t index = 0; index < kGraphicsStages; ++index) {
        const std::vector<std::uint8_t>* dxil = stages[index];
        mix_u32(hash, dxil != nullptr ? 1u : 0u);
        if (dxil != nullptr) {
            mix_text(hash, kStageNames[index]);
            mix_bytes(hash, dxil->data(), dxil->size());
        }
    }
    mix_blend(hash, built.desc.BlendState);
    mix_u32(hash, built.desc.SampleMask);
    mix_rasterizer(hash, built.desc.RasterizerState);
    mix_depth(hash, built.desc.DepthStencilState);
    mix_u32(hash, static_cast<std::uint32_t>(built.desc.IBStripCutValue));
    mix_u32(hash, static_cast<std::uint32_t>(built.desc.PrimitiveTopologyType));
    mix_u32(hash, built.desc.NumRenderTargets);
    for (std::size_t index = 0; index < kMaxRenderTargets; ++index) {
        mix_u32(hash, static_cast<std::uint32_t>(built.desc.RTVFormats[index]));
    }
    mix_u32(hash, static_cast<std::uint32_t>(built.desc.DSVFormat));
    mix_u32(hash, built.desc.SampleDesc.Count);
    mix_u32(hash, built.desc.SampleDesc.Quality);
    mix_u32(hash, built.desc.NodeMask);
    mix_u32(hash, static_cast<std::uint32_t>(built.desc.Flags));
    mix_u32(hash, built.desc.InputLayout.NumElements);
    for (std::size_t index = 0; index < built.elements.size(); ++index) {
        const D3D12_INPUT_ELEMENT_DESC& element = built.elements[index];
        mix_text(hash, built.names[index]);
        mix_u32(hash, element.SemanticIndex);
        mix_u32(hash, static_cast<std::uint32_t>(element.Format));
        mix_u32(hash, element.InputSlot);
        mix_u32(hash, element.AlignedByteOffset);
        mix_u32(hash, static_cast<std::uint32_t>(element.InputSlotClass));
        mix_u32(hash, element.InstanceDataStepRate);
    }
    mix_root(hash, root_blob, root);
    return hash;
}

Hash compute_key(const std::vector<std::uint8_t>& dxil, const std::vector<std::uint8_t>& root_blob,
                 const void* root)
{
    Hash hash = kHashOffset;
    mix_u32(hash, 0x43505053u);
    mix_bytes(hash, dxil.data(), dxil.size());
    mix_root(hash, root_blob, root);
    return hash;
}

bool plausible_root(const RootSignatureDesc& desc)
{
    return desc.parameter_count <= 64 && desc.sampler_count <= 64
        && (desc.parameter_count == 0 || desc.parameters != nullptr)
        && (desc.sampler_count == 0 || desc.samplers != nullptr);
}

RootSignatureDesc read_root_desc(const std::uint8_t* object, bool natural_layout)
{
    RootSignatureDesc desc{};
    const std::size_t parameters = natural_layout ? 12 : 8;
    const std::size_t sampler_count = natural_layout ? 20 : 16;
    const std::size_t samplers = natural_layout ? 28 : 20;
    const std::size_t flags = natural_layout ? 36 : 28;
    std::memcpy(&desc.parameter_count, object + 4, sizeof(desc.parameter_count));
    std::memcpy(&desc.parameters, object + parameters, sizeof(desc.parameters));
    std::memcpy(&desc.sampler_count, object + sampler_count, sizeof(desc.sampler_count));
    std::memcpy(&desc.samplers, object + samplers, sizeof(desc.samplers));
    std::memcpy(&desc.flags, object + flags, sizeof(desc.flags));
    return desc;
}

std::string graphics_summary(const HostGraphics& built, const bool present[kGraphicsStages],
                             std::size_t bytes[kGraphicsStages])
{
    std::string text = "stages=";
    for (std::size_t index = 0; index < kGraphicsStages; ++index) {
        if (index != 0) {
            text += ',';
        }
        if (present[index]) {
            text += kStageNames[index];
            text += '(';
            text += std::to_string(bytes[index]);
            text += ')';
        } else {
            text += '-';
        }
    }
    text += " rt=" + std::to_string(built.desc.NumRenderTargets);
    text += " rt0=" + std::to_string(static_cast<std::uint32_t>(built.desc.RTVFormats[0]));
    text += " dsv=" + std::to_string(static_cast<std::uint32_t>(built.desc.DSVFormat));
    text += " topo=" + std::to_string(static_cast<std::uint32_t>(built.desc.PrimitiveTopologyType));
    text += " cull=" + std::to_string(static_cast<std::uint32_t>(built.desc.RasterizerState.CullMode));
    text += " depth=" + std::to_string(static_cast<std::uint32_t>(built.desc.DepthStencilState.DepthEnable));
    text += " inputs=" + std::to_string(built.elements.size());
    return text;
}

HRESULT make_root_signature(ID3D12Device* host, const RootSignatureDesc* desc,
                            RootSignatureVersion version, ID3D12RootSignature** out,
                            std::vector<std::uint8_t>& blob_out)
{
    if (host == nullptr || desc == nullptr || out == nullptr) {
        return E_INVALIDARG;
    }
    *out = nullptr;
    blob_out.clear();

    ID3DBlob* blob = nullptr;
    ID3DBlob* error = nullptr;
    const HRESULT serialized = serialize_root_signature(desc, version, &blob, &error);
    if (FAILED(serialized) || blob == nullptr) {
        log::warn("root signature: serialize failed hr=0x" + hr_text(serialized) + " "
                  + trim_text(blob_text(error)));
        if (blob != nullptr) {
            blob->Release();
        }
        if (error != nullptr) {
            error->Release();
        }
        return FAILED(serialized) ? serialized : E_FAIL;
    }

    const void* data = blob->GetBufferPointer();
    const SIZE_T size = blob->GetBufferSize();
    if (data == nullptr || size == 0) {
        log::warn("root signature: host serializer returned no bytes");
        blob->Release();
        if (error != nullptr) {
            error->Release();
        }
        return E_FAIL;
    }

    blob_out.assign(static_cast<const std::uint8_t*>(data),
                    static_cast<const std::uint8_t*>(data) + size);
    const HRESULT created =
        host->CreateRootSignature(0, data, size, IID_PPV_ARGS(out));
    if (FAILED(created)) {
        log::warn("root signature: CreateRootSignature failed hr=0x" + hr_text(created) + " "
                  + trim_text(blob_text(error)));
        *out = nullptr;
    } else {
        log::info("root signature: created bytes=" + std::to_string(size)
                  + " flags=" + hex32(desc->flags & ~kConsoleRootSignatureFlags));
    }

    blob->Release();
    if (error != nullptr) {
        error->Release();
    }
    return created;
}

HRESULT empty_root_signature(ID3D12Device* host, bool compute, ID3D12RootSignature** out,
                             std::vector<std::uint8_t>& blob_out)
{
    RootSignatureDesc desc{};
    std::uint32_t flags = static_cast<std::uint32_t>(D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED)
        | static_cast<std::uint32_t>(D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED);
    if (!compute) {
        flags |= static_cast<std::uint32_t>(D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    }
    desc.flags = flags;
    log::warn("pipeline: no console root signature; using an empty host root signature");
    return make_root_signature(host, &desc, RootSignatureVersion::Version11, out, blob_out);
}

bool cache_lookup(Hash key, Pipeline& out)
{
    std::lock_guard<std::mutex> lock(g_pipeline_mutex);
    const auto entry = g_pipelines.find(key);
    if (entry == g_pipelines.end()) {
        g_pipeline_misses.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    entry->second.state->AddRef();
    entry->second.root->AddRef();
    out.state = entry->second.state;
    out.root = entry->second.root;
    out.key = key;
    g_pipeline_hits.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void cache_store(Hash key, ID3D12PipelineState* state, ID3D12RootSignature* root)
{
    std::lock_guard<std::mutex> lock(g_pipeline_mutex);
    const auto [entry, inserted] = g_pipelines.emplace(key, PipelineEntry{state, root});
    if (inserted) {
        state->AddRef();
        root->AddRef();
    }
}

HRESULT make_graphics(void* owner, ID3D12Device* host, const GraphicsHead* head,
                      const D3D12_GRAPHICS_PIPELINE_STATE_DESC& console_state,
                      const RootSignatureDesc* console_root, RootSignatureVersion root_version,
                      Pipeline* out)
{
    if (host == nullptr || head == nullptr || out == nullptr) {
        log::error("pipeline graphics: null device, head or output");
        return E_INVALIDARG;
    }
    out->state = nullptr;
    out->root = nullptr;
    out->key = 0;

    std::array<const std::vector<std::uint8_t>*, kGraphicsStages> stages{};
    bool present[kGraphicsStages]{};
    std::size_t bytes[kGraphicsStages]{};
    std::size_t present_count = 0;
    std::size_t total_bytes = 0;

    for (std::size_t index = 0; index < kGraphicsStages; ++index) {
        const Bytecode& code = head->stages[index];
        if (code.code == nullptr || code.size == 0) {
            continue;
        }
        present[index] = true;
        ++present_count;
        bytes[index] = code.size;
        total_bytes += code.size;

        Hash hash = 0;
        const Translation* translation = translated_for(code, hash);
        if (translation == nullptr || !translation->ok) {
            log::warn(std::string("pipeline graphics: ") + kStageNames[index]
                      + " shader does not translate hash=" + hex64(hash) + " size="
                      + std::to_string(code.size));
            return E_FAIL;
        }
        stages[index] = &translation->dxil;
    }

    if (present_count == 0) {
        log::warn("pipeline graphics: no shader stages; owner=" + pointer_text(owner));
        return E_INVALIDARG;
    }

    ID3D12RootSignature* root = nullptr;
    std::vector<std::uint8_t> root_blob;
    if (console_root != nullptr && plausible_root(*console_root)) {
        const HRESULT created = make_root_signature(host, console_root, root_version, &root, root_blob);
        if (FAILED(created)) {
            log::warn("pipeline graphics: console root signature refused hr=0x" + hr_text(created));
        }
    }
    if (root == nullptr && console_state.pRootSignature != nullptr) {
        root = console_state.pRootSignature;
        root->AddRef();
        log::info("pipeline graphics: using the host root signature from the console state");
    }
    if (root == nullptr && head->root != nullptr) {
        root = static_cast<ID3D12RootSignature*>(head->root);
        root->AddRef();
        log::info("pipeline graphics: using the host root signature from the console head");
    }
    if (root == nullptr) {
        const HRESULT created = empty_root_signature(host, false, &root, root_blob);
        if (FAILED(created) || root == nullptr) {
            log::error("pipeline graphics: no host root signature hr=0x" + hr_text(created));
            return FAILED(created) ? created : E_FAIL;
        }
    }

    HostGraphics built;
    if (!build_graphics_state(console_state, built)) {
        log::error("pipeline graphics: cannot translate the console graphics state");
        root->Release();
        return E_INVALIDARG;
    }
    built.desc.pRootSignature = root;

    const auto bytecode_of = [&stages](std::size_t index) {
        D3D12_SHADER_BYTECODE bytecode{};
        if (stages[index] != nullptr) {
            bytecode.pShaderBytecode = stages[index]->data();
            bytecode.BytecodeLength = stages[index]->size();
        }
        return bytecode;
    };
    built.desc.VS = bytecode_of(0);
    built.desc.PS = bytecode_of(1);
    built.desc.DS = bytecode_of(2);
    built.desc.HS = bytecode_of(3);
    built.desc.GS = bytecode_of(4);

    const Hash key = graphics_key(built, stages, root_blob, root);
    Pipeline cached{};
    if (cache_lookup(key, cached)) {
        log::info("pipeline graphics: cache hit key=" + hex64(key) + " owner=" + pointer_text(owner)
                  + " " + graphics_summary(built, present, bytes));
        root->Release();
        *out = cached;
        return S_OK;
    }

    ID3D12PipelineState* state = nullptr;
    const HRESULT created = host->CreateGraphicsPipelineState(&built.desc, IID_PPV_ARGS(&state));
    if (FAILED(created) || state == nullptr) {
        log::warn("pipeline graphics: CreateGraphicsPipelineState failed hr=0x" + hr_text(created)
                  + " key=" + hex64(key) + " " + graphics_summary(built, present, bytes));
        root->Release();
        return FAILED(created) ? created : E_FAIL;
    }

    cache_store(key, state, root);
    out->state = state;
    out->root = root;
    out->key = key;
    log::info("pipeline graphics: created key=" + hex64(key) + " owner=" + pointer_text(owner)
              + " bytes=" + std::to_string(total_bytes) + " " + graphics_summary(built, present, bytes)
              + " hr=0x" + hr_text(created));
    return S_OK;
}

HRESULT make_compute(void* owner, ID3D12Device* host, const ComputeHead* head,
                     const RootSignatureDesc* console_root, RootSignatureVersion root_version,
                     Pipeline* out)
{
    if (host == nullptr || head == nullptr || out == nullptr) {
        log::error("pipeline compute: null device, head or output");
        return E_INVALIDARG;
    }
    out->state = nullptr;
    out->root = nullptr;
    out->key = 0;

    if (head->stage.code == nullptr || head->stage.size == 0) {
        log::warn("pipeline compute: no compute shader; owner=" + pointer_text(owner));
        return E_INVALIDARG;
    }

    Hash shader_hash = 0;
    const Translation* translation = translated_for(head->stage, shader_hash);
    if (translation == nullptr || !translation->ok) {
        log::warn("pipeline compute: compute shader does not translate hash=" + hex64(shader_hash)
                  + " size=" + std::to_string(head->stage.size));
        return E_FAIL;
    }

    ID3D12RootSignature* root = nullptr;
    std::vector<std::uint8_t> root_blob;
    if (console_root != nullptr && plausible_root(*console_root)) {
        const HRESULT created = make_root_signature(host, console_root, root_version, &root, root_blob);
        if (FAILED(created)) {
            log::warn("pipeline compute: console root signature refused hr=0x" + hr_text(created));
        }
    }
    if (root == nullptr && head->root != nullptr) {
        root = static_cast<ID3D12RootSignature*>(head->root);
        root->AddRef();
        log::info("pipeline compute: using the host root signature from the console head");
    }
    if (root == nullptr) {
        const HRESULT created = empty_root_signature(host, true, &root, root_blob);
        if (FAILED(created) || root == nullptr) {
            log::error("pipeline compute: no host root signature hr=0x" + hr_text(created));
            return FAILED(created) ? created : E_FAIL;
        }
    }

    const Hash key = compute_key(translation->dxil, root_blob, root);
    Pipeline cached{};
    if (cache_lookup(key, cached)) {
        log::info("pipeline compute: cache hit key=" + hex64(key) + " owner=" + pointer_text(owner)
                  + " cs=" + std::to_string(translation->dxil.size()));
        root->Release();
        *out = cached;
        return S_OK;
    }

    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.CS.pShaderBytecode = translation->dxil.data();
    desc.CS.BytecodeLength = translation->dxil.size();
    desc.NodeMask = 0;
    desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    ID3D12PipelineState* state = nullptr;
    const HRESULT created = host->CreateComputePipelineState(&desc, IID_PPV_ARGS(&state));
    if (FAILED(created) || state == nullptr) {
        log::warn("pipeline compute: CreateComputePipelineState failed hr=0x" + hr_text(created)
                  + " key=" + hex64(key) + " cs=" + std::to_string(translation->dxil.size()));
        root->Release();
        return FAILED(created) ? created : E_FAIL;
    }

    cache_store(key, state, root);
    out->state = state;
    out->root = root;
    out->key = key;
    log::info("pipeline compute: created key=" + hex64(key) + " owner=" + pointer_text(owner)
              + " cs=" + std::to_string(translation->dxil.size()) + " hr=0x" + hr_text(created));
    return S_OK;
}

enum class SubKind : std::uint8_t {
    Unknown,
    Ignore,
    Root,
    Vertex,
    Pixel,
    Domain,
    Hull,
    Geometry,
    Compute,
    Amplification,
    Mesh,
    Blend,
    SampleMask,
    Rasterizer,
    Depth,
    Depth1,
    InputLayout,
    StripCut,
    Topology,
    RtFormats,
    DsvFormat,
    Samples,
};

struct Subobject {
    std::size_t size;
    SubKind kind;
};

Subobject subobject_of(std::uint32_t kind)
{
    switch (kind) {
    case 0:
        return {kSubobjectUnknown, SubKind::Ignore};
    case 1:
        return {kSubobjectStage, SubKind::Vertex};
    case 2:
        return {kSubobjectStage, SubKind::Pixel};
    case 3:
        return {kSubobjectStage, SubKind::Domain};
    case 4:
        return {kSubobjectStage, SubKind::Hull};
    case 5:
        return {kSubobjectStage, SubKind::Geometry};
    case 6:
        return {kSubobjectStage, SubKind::Compute};
    case 7:
        return {kSubobjectRootSignature, SubKind::Root};
    case 8:
        return {kSubobjectBlend, SubKind::Blend};
    case 9:
        return {kSubobjectScalar, SubKind::SampleMask};
    case 10:
        return {kSubobjectRasterizer, SubKind::Rasterizer};
    case 11:
        return {kSubobjectDepth, SubKind::Depth};
    case 12:
        return {kSubobjectInputLayout, SubKind::InputLayout};
    case 13:
        return {kSubobjectScalar, SubKind::StripCut};
    case 14:
        return {kSubobjectScalar, SubKind::Topology};
    case 15:
        return {kSubobjectRtFormats, SubKind::RtFormats};
    case 16:
        return {kSubobjectScalar, SubKind::DsvFormat};
    case 17:
        return {kSubobjectSamples, SubKind::Samples};
    case 18:
    case 20:
        return {kSubobjectScalar, SubKind::Ignore};
    case 19:
        return {kSubobjectStage, SubKind::Ignore};
    case 21:
        return {kSubobjectDepth1, SubKind::Depth1};
    case 22:
        return {kSubobjectOpaque, SubKind::Ignore};
    case 23:
        return {kSubobjectStage, SubKind::Amplification};
    case 24:
        return {kSubobjectStage, SubKind::Mesh};
    default:
        return {0, SubKind::Unknown};
    }
}

struct StreamContent {
    std::array<Bytecode, kGraphicsStages> stages{};
    bool present[kGraphicsStages]{};
    Bytecode compute{};
    bool compute_present{};
    bool mesh_present{};
    bool amplification_present{};
    bool root_present{};
    RootSignatureDesc root{};
    D3D12_GRAPHICS_PIPELINE_STATE_DESC state{};
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<std::string> names;
    bool valid{true};
};

void read_stage(StreamContent& content, SubKind kind, const std::uint8_t* object)
{
    Bytecode code{};
    std::memcpy(&code, object + 8, sizeof(code));
    if (code.code == nullptr || code.size == 0) {
        return;
    }
    switch (kind) {
    case SubKind::Vertex:
        content.stages[0] = code;
        content.present[0] = true;
        break;
    case SubKind::Pixel:
        content.stages[1] = code;
        content.present[1] = true;
        break;
    case SubKind::Domain:
        content.stages[2] = code;
        content.present[2] = true;
        break;
    case SubKind::Hull:
        content.stages[3] = code;
        content.present[3] = true;
        break;
    case SubKind::Geometry:
        content.stages[4] = code;
        content.present[4] = true;
        break;
    case SubKind::Compute:
        content.compute = code;
        content.compute_present = true;
        break;
    case SubKind::Mesh:
        content.mesh_present = true;
        break;
    case SubKind::Amplification:
        content.amplification_present = true;
        break;
    default:
        break;
    }
}

void read_input_layout(StreamContent& content, const std::uint8_t* object)
{
    std::uint32_t count = 0;
    const void* pointer = nullptr;
    std::memcpy(&count, object + 8, sizeof(count));
    std::memcpy(&pointer, object + 16, sizeof(pointer));
    if (count == 0) {
        return;
    }
    if (pointer == nullptr || count > 256) {
        log::warn("pipeline stream: input layout count=" + std::to_string(count)
                  + " is not usable; the layout is dropped");
        return;
    }
    const auto* base = static_cast<const std::uint8_t*>(pointer);
    content.elements.reserve(count);
    content.names.reserve(count);
    for (std::uint32_t index = 0; index < count; ++index) {
        D3D12_INPUT_ELEMENT_DESC element{};
        std::memcpy(&element, base + static_cast<std::size_t>(index) * sizeof(element),
                    sizeof(element));
        if (element.SemanticName == nullptr) {
            log::warn("pipeline stream: input element without a semantic name");
            content.valid = false;
            return;
        }
        content.names.emplace_back(element.SemanticName);
        element.SemanticName = nullptr;
        content.elements.push_back(element);
    }
    for (std::size_t index = 0; index < content.elements.size(); ++index) {
        content.elements[index].SemanticName = content.names[index].c_str();
    }
    content.state.InputLayout.NumElements = static_cast<UINT>(content.elements.size());
    content.state.InputLayout.pInputElementDescs = content.elements.data();
}

void read_stream_object(StreamContent& content, Subobject spec, const std::uint8_t* object)
{
    switch (spec.kind) {
    case SubKind::Root: {
        RootSignatureDesc desc = read_root_desc(object, true);
        if (!plausible_root(desc)) {
            desc = read_root_desc(object, false);
        }
        if (plausible_root(desc)) {
            content.root = desc;
            content.root_present = true;
            log::debug("pipeline stream: root signature parameters="
                       + std::to_string(desc.parameter_count) + " samplers="
                       + std::to_string(desc.sampler_count) + " flags=" + hex32(desc.flags));
        } else {
            log::warn("pipeline stream: root signature description is not readable");
        }
        break;
    }
    case SubKind::Blend: {
        D3D12_BLEND_DESC blend{};
        std::memcpy(&blend, object + 4, sizeof(blend));
        content.state.BlendState = blend;
        break;
    }
    case SubKind::SampleMask:
        std::memcpy(&content.state.SampleMask, object + 4, sizeof(content.state.SampleMask));
        break;
    case SubKind::Rasterizer: {
        D3D12_RASTERIZER_DESC rasterizer{};
        std::memcpy(&rasterizer, object + 4, sizeof(rasterizer));
        content.state.RasterizerState = rasterizer;
        break;
    }
    case SubKind::Depth: {
        D3D12_DEPTH_STENCIL_DESC depth{};
        std::memcpy(&depth, object + 4, sizeof(depth));
        content.state.DepthStencilState = depth;
        break;
    }
    case SubKind::Depth1: {
        D3D12_DEPTH_STENCIL_DESC1 depth{};
        std::memcpy(&depth, object + 4, sizeof(depth));
        D3D12_DEPTH_STENCIL_DESC plain{};
        plain.DepthEnable = depth.DepthEnable;
        plain.DepthWriteMask = depth.DepthWriteMask;
        plain.DepthFunc = depth.DepthFunc;
        plain.StencilEnable = depth.StencilEnable;
        plain.StencilReadMask = depth.StencilReadMask;
        plain.StencilWriteMask = depth.StencilWriteMask;
        plain.FrontFace = depth.FrontFace;
        plain.BackFace = depth.BackFace;
        content.state.DepthStencilState = plain;
        break;
    }
    case SubKind::InputLayout:
        read_input_layout(content, object);
        break;
    case SubKind::StripCut: {
        std::uint32_t value = 0;
        std::memcpy(&value, object + 4, sizeof(value));
        content.state.IBStripCutValue = static_cast<D3D12_INDEX_BUFFER_STRIP_CUT_VALUE>(value);
        break;
    }
    case SubKind::Topology: {
        std::uint32_t value = 0;
        std::memcpy(&value, object + 4, sizeof(value));
        content.state.PrimitiveTopologyType = static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(value);
        break;
    }
    case SubKind::RtFormats: {
        D3D12_RT_FORMAT_ARRAY formats{};
        std::memcpy(&formats, object + 4, sizeof(formats));
        content.state.NumRenderTargets = formats.NumRenderTargets;
        for (std::size_t index = 0; index < kMaxRenderTargets; ++index) {
            content.state.RTVFormats[index] = formats.RTFormats[index];
        }
        break;
    }
    case SubKind::DsvFormat: {
        std::uint32_t value = 0;
        std::memcpy(&value, object + 4, sizeof(value));
        content.state.DSVFormat = static_cast<DXGI_FORMAT>(value);
        break;
    }
    case SubKind::Samples:
        std::memcpy(&content.state.SampleDesc, object + 4, sizeof(content.state.SampleDesc));
        break;
    default:
        break;
    }
}

StreamContent read_stream(const std::uint8_t* stream, std::size_t size)
{
    StreamContent content;
    content.state = default_graphics_state();
    std::size_t at = 0;
    while (at + 8 <= size) {
        std::uint32_t kind = 0;
        std::memcpy(&kind, stream + at, sizeof(kind));
        const Subobject spec = subobject_of(kind);
        if (spec.kind == SubKind::Unknown) {
            log::warn("pipeline stream: unknown subobject kind=" + std::to_string(kind)
                      + " at=" + std::to_string(at) + "; the rest is ignored");
            break;
        }
        if (at + spec.size > size) {
            log::warn("pipeline stream: subobject kind=" + std::to_string(kind)
                      + " at=" + std::to_string(at) + " is truncated");
            break;
        }
        const std::uint8_t* object = stream + at;
        read_stream_object(content, spec, object);
        switch (spec.kind) {
        case SubKind::Vertex:
        case SubKind::Pixel:
        case SubKind::Domain:
        case SubKind::Hull:
        case SubKind::Geometry:
        case SubKind::Compute:
        case SubKind::Amplification:
        case SubKind::Mesh:
            read_stage(content, spec.kind, object);
            break;
        default:
            break;
        }
        if (!content.valid) {
            break;
        }
        at += spec.size;
    }
    return content;
}

}

HRESULT serialize_root_signature(const RootSignatureDesc* desc, RootSignatureVersion version,
                                 ID3DBlob** blob, ID3DBlob** error)
{
    if (blob == nullptr || error == nullptr) {
        return E_POINTER;
    }
    *blob = nullptr;
    *error = nullptr;
    if (desc == nullptr) {
        return E_INVALIDARG;
    }
    if ((desc->parameter_count != 0 && desc->parameters == nullptr)
        || (desc->sampler_count != 0 && desc->samplers == nullptr)) {
        log::warn("root signature: counts name missing arrays");
        return E_INVALIDARG;
    }

    const std::uint32_t flags = desc->flags & ~kConsoleRootSignatureFlags;
    if (flags != desc->flags) {
        log::debug("root signature: console flags " + hex32(desc->flags) + " stripped to "
                   + hex32(flags));
    }

    D3D12_ROOT_SIGNATURE_DESC host{};
    host.NumParameters = desc->parameter_count;
    host.pParameters = static_cast<const D3D12_ROOT_PARAMETER*>(desc->parameters);
    host.NumStaticSamplers = desc->sampler_count;
    host.pStaticSamplers = static_cast<const D3D12_STATIC_SAMPLER_DESC*>(desc->samplers);
    host.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(flags);

    if (version == RootSignatureVersion::Version11) {
        D3D12_VERSIONED_ROOT_SIGNATURE_DESC versioned{};
        versioned.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        versioned.Desc_1_1.NumParameters = host.NumParameters;
        versioned.Desc_1_1.pParameters =
            reinterpret_cast<const D3D12_ROOT_PARAMETER1*>(host.pParameters);
        versioned.Desc_1_1.NumStaticSamplers = host.NumStaticSamplers;
        versioned.Desc_1_1.pStaticSamplers = host.pStaticSamplers;
        versioned.Desc_1_1.Flags = host.Flags;
        const SerializeVersioned function =
            host_export<SerializeVersioned>("D3D12SerializeVersionedRootSignature");
        if (function != nullptr) {
            return function(&versioned, blob, error);
        }
        return D3D12SerializeVersionedRootSignature(&versioned, blob, error);
    }

    const Serialize10 function = host_export<Serialize10>("D3D12SerializeRootSignature");
    if (function != nullptr) {
        return function(&host, D3D_ROOT_SIGNATURE_VERSION_1_0, blob, error);
    }
    return D3D12SerializeRootSignature(&host, D3D_ROOT_SIGNATURE_VERSION_1_0, blob, error);
}

HRESULT serialize_versioned_root_signature(const VersionedRootSignatureDesc* desc,
                                           ID3DBlob** blob, ID3DBlob** error)
{
    if (blob == nullptr || error == nullptr) {
        return E_POINTER;
    }
    *blob = nullptr;
    *error = nullptr;
    if (desc == nullptr) {
        return E_INVALIDARG;
    }
    if ((desc->desc.parameter_count != 0 && desc->desc.parameters == nullptr)
        || (desc->desc.sampler_count != 0 && desc->desc.samplers == nullptr)) {
        log::warn("root signature: versioned counts name missing arrays");
        return E_INVALIDARG;
    }

    const std::uint32_t flags = desc->desc.flags & ~kConsoleRootSignatureFlags;
    if (flags != desc->desc.flags) {
        log::debug("root signature: console flags " + hex32(desc->desc.flags) + " stripped to "
                   + hex32(flags));
    }

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC host{};
    if (desc->version >= 2) {
        host.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
        host.Desc_1_1.NumParameters = desc->desc.parameter_count;
        host.Desc_1_1.pParameters =
            static_cast<const D3D12_ROOT_PARAMETER1*>(desc->desc.parameters);
        host.Desc_1_1.NumStaticSamplers = desc->desc.sampler_count;
        host.Desc_1_1.pStaticSamplers =
            static_cast<const D3D12_STATIC_SAMPLER_DESC*>(desc->desc.samplers);
        host.Desc_1_1.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(flags);
    } else {
        host.Version = D3D_ROOT_SIGNATURE_VERSION_1_0;
        host.Desc_1_0.NumParameters = desc->desc.parameter_count;
        host.Desc_1_0.pParameters =
            static_cast<const D3D12_ROOT_PARAMETER*>(desc->desc.parameters);
        host.Desc_1_0.NumStaticSamplers = desc->desc.sampler_count;
        host.Desc_1_0.pStaticSamplers =
            static_cast<const D3D12_STATIC_SAMPLER_DESC*>(desc->desc.samplers);
        host.Desc_1_0.Flags = static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(flags);
    }

    const SerializeVersioned function =
        host_export<SerializeVersioned>("D3D12SerializeVersionedRootSignature");
    if (function != nullptr) {
        return function(&host, blob, error);
    }
    return D3D12SerializeVersionedRootSignature(&host, blob, error);
}

HRESULT create_root_signature(ID3D12Device* host, const RootSignatureDesc* desc,
                              RootSignatureVersion version, ID3D12RootSignature** out)
{
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    if (host == nullptr || desc == nullptr) {
        return E_INVALIDARG;
    }
    std::vector<std::uint8_t> blob;
    return make_root_signature(host, desc, version, out, blob);
}

HRESULT create_graphics_pipeline(void* owner, ID3D12Device* host, const GraphicsHead* head,
                                 const D3D12_GRAPHICS_PIPELINE_STATE_DESC* console_state,
                                 const RootSignatureDesc* console_root, Pipeline* out,
                                 RootSignatureVersion root_version)
{
    if (console_state == nullptr) {
        log::error("pipeline graphics: null console state");
        if (out != nullptr) {
            out->state = nullptr;
            out->root = nullptr;
            out->key = 0;
        }
        return E_INVALIDARG;
    }
    return make_graphics(owner, host, head, *console_state, console_root, root_version, out);
}

HRESULT create_compute_pipeline(void* owner, ID3D12Device* host, const ComputeHead* head,
                                const RootSignatureDesc* console_root, Pipeline* out,
                                RootSignatureVersion root_version)
{
    return make_compute(owner, host, head, console_root, root_version, out);
}

HRESULT create_compute_pipeline_x(void* owner, ID3D12Device* host, const ComputeHead* head,
                                  std::uint32_t extension_count, const void* extensions,
                                  const RootSignatureDesc* console_root, Pipeline* out,
                                  RootSignatureVersion root_version)
{
    if (extension_count != 0 || extensions != nullptr) {
        log::info("pipeline compute: " + std::to_string(extension_count)
                  + " console extensions ignored");
    }
    return make_compute(owner, host, head, console_root, root_version, out);
}

HRESULT create_pipeline_from_stream(void* owner, ID3D12Device* host, const StreamDesc* desc,
                                    Pipeline* out)
{
    if (out == nullptr) {
        return E_POINTER;
    }
    out->state = nullptr;
    out->root = nullptr;
    out->key = 0;
    if (host == nullptr || desc == nullptr || desc->stream == nullptr || desc->size == 0) {
        log::error("pipeline stream: null device, description or stream");
        return E_INVALIDARG;
    }

    const auto* stream = static_cast<const std::uint8_t*>(desc->stream);
    StreamContent content = read_stream(stream, desc->size);
    if (!content.valid) {
        log::error("pipeline stream: malformed input layout");
        return E_INVALIDARG;
    }

    const RootSignatureDesc* root = content.root_present ? &content.root : nullptr;
    bool any_graphics = false;
    for (std::size_t index = 0; index < kGraphicsStages; ++index) {
        any_graphics = any_graphics || content.present[index];
    }

    if (content.compute_present && !any_graphics) {
        ComputeHead head{};
        head.root = nullptr;
        head.stage = content.compute;
        log::info("pipeline stream: compute size=" + std::to_string(desc->size)
                  + " root=" + std::string(content.root_present ? "console" : "none"));
        return make_compute(owner, host, &head, root,
                            RootSignatureVersion::Version10, out);
    }

    if (!any_graphics) {
        if (content.mesh_present || content.amplification_present) {
            log::warn("pipeline stream: mesh and amplification stages are not translated");
            return E_NOTIMPL;
        }
        log::warn("pipeline stream: no shader stages");
        return E_INVALIDARG;
    }

    GraphicsHead head{};
    head.root = nullptr;
    for (std::size_t index = 0; index < kGraphicsStages; ++index) {
        head.stages[index] = content.stages[index];
    }

    std::string presence;
    for (std::size_t index = 0; index < kGraphicsStages; ++index) {
        if (index != 0) {
            presence += ',';
        }
        presence += content.present[index] ? kStageNames[index] : "-";
    }
    log::info("pipeline stream: graphics size=" + std::to_string(desc->size) + " stages="
              + presence + " root=" + std::string(content.root_present ? "console" : "none")
              + " inputs=" + std::to_string(content.elements.size()));

    return make_graphics(owner, host, &head, content.state, root,
                         RootSignatureVersion::Version10, out);
}

void release_pipeline(Pipeline& pipeline) noexcept
{
    if (pipeline.state != nullptr) {
        pipeline.state->Release();
    }
    if (pipeline.root != nullptr) {
        pipeline.root->Release();
    }
    pipeline.state = nullptr;
    pipeline.root = nullptr;
    pipeline.key = 0;
}

void clear_pipeline_cache()
{
    std::unordered_map<Hash, PipelineEntry> removed;
    {
        std::lock_guard<std::mutex> lock(g_pipeline_mutex);
        removed.swap(g_pipelines);
    }
    for (const auto& [key, entry] : removed) {
        if (entry.state != nullptr) {
            entry.state->Release();
        }
        if (entry.root != nullptr) {
            entry.root->Release();
        }
    }
    log::info("pipeline cache: cleared " + std::to_string(removed.size()) + " entries");
}

std::uint64_t pipeline_cache_hits() noexcept
{
    return g_pipeline_hits.load(std::memory_order_relaxed);
}

std::uint64_t pipeline_cache_misses() noexcept
{
    return g_pipeline_misses.load(std::memory_order_relaxed);
}

std::uint64_t shader_translation_hits() noexcept
{
    return g_translation_hits.load(std::memory_order_relaxed);
}

std::uint64_t shader_translation_misses() noexcept
{
    return g_translation_misses.load(std::memory_order_relaxed);
}

std::size_t translated_shader_count() noexcept
{
    std::lock_guard<std::mutex> lock(g_translation_mutex);
    return g_translations.size();
}

std::size_t cached_pipeline_count() noexcept
{
    std::lock_guard<std::mutex> lock(g_pipeline_mutex);
    return g_pipelines.size();
}

const char* stage_name(PipelineStage stage) noexcept
{
    switch (stage) {
    case PipelineStage::Vertex:
        return "vertex";
    case PipelineStage::Pixel:
        return "pixel";
    case PipelineStage::Domain:
        return "domain";
    case PipelineStage::Hull:
        return "hull";
    case PipelineStage::Geometry:
        return "geometry";
    case PipelineStage::Compute:
        return "compute";
    case PipelineStage::Amplification:
        return "amplification";
    case PipelineStage::Mesh:
        return "mesh";
    case PipelineStage::Unknown:
        return "unknown";
    }
    return "unknown";
}

std::span<const std::uint8_t> translate_shader(const Bytecode& bytecode)
{
    if (bytecode.code == nullptr || bytecode.size == 0) {
        return {};
    }
    Hash hash = 0;
    const Translation* translation = translated_for(bytecode, hash);
    if (translation == nullptr || !translation->ok) {
        return {};
    }
    return std::span<const std::uint8_t>(translation->dxil.data(), translation->dxil.size());
}

}
