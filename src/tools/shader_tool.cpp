#include <euphoria/formats/bcpack.hpp>
#include <euphoria/formats/inflate.hpp>
#include <euphoria/gpu/memory.hpp>
#include <euphoria/shim/dstorage.hpp>
#include <euphoria/shim/xmem.hpp>
#include <euphoria/shader/aot.hpp>
#include <euphoria/shader/cache.hpp>
#include <euphoria/shader/dxc.hpp>
#include <euphoria/shader/lift_hlsl.hpp>
#include <euphoria/shader/xbox.hpp>
#include <euphoria/shader/gfx10/decode.hpp>
#include <euphoria/shader/gfx10/ir.hpp>
#include <euphoria/shader/gfx10/selftest.hpp>
#include <euphoria/shader/gfx10/dxbc.hpp>
#include <euphoria/shader/gfx10/native.hpp>
#include <euphoria/shader/gfx10/spill.hpp>
#include <euphoria/shader/gfx10/structure.hpp>

#include <openscarlett/gpu/rdna2_disasm.h>
#include <euphoria/shader/codec.hpp>
#include <euphoria/shader/container.hpp>
#include <euphoria/shader/translator.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

std::vector<std::uint8_t> read_file(const char* path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        return {};
    }
    const std::streamoff size = input.tellg();
    if (size <= 0) {
        return {};
    }
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(data.data()), size);
    return input ? data : std::vector<std::uint8_t>{};
}

bool write_file(const char* path, const std::vector<std::uint8_t>& data)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return false;
    }
    output.write(reinterpret_cast<const char*>(data.data()),
                 static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(output);
}

int selftest()
{
    std::vector<std::uint32_t> generator{0x12345678u};    const auto next = [&generator]() {
        generator[0] ^= generator[0] << 13;
        generator[0] ^= generator[0] >> 17;
        generator[0] ^= generator[0] << 5;
        return generator[0];
    };

    std::vector<std::vector<std::uint8_t>> cases;
    cases.push_back({});
    cases.push_back({'a'});
    cases.push_back({'h', 'e', 'l', 'l', 'o'});
    cases.push_back(std::vector<std::uint8_t>(100000, 0));
    cases.push_back([] {
        std::vector<std::uint8_t> data(1U << 20);
        for (std::size_t index = 0; index < data.size(); ++index) {
            data[index] = static_cast<std::uint8_t>(index);
        }
        return data;
    }());
    cases.push_back([&next] {
        std::vector<std::uint8_t> data(1U << 20);
        for (std::size_t index = 0; index < data.size(); ++index) {
            data[index] = static_cast<std::uint8_t>(next());
        }
        return data;
    }());

    int failures = 0;
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto& source = cases[index];
        const std::vector<std::uint8_t> stored = euphoria::shader::encode(source);
        const std::optional<std::vector<std::uint8_t>> restored =
            euphoria::shader::decode(stored);
        const bool ok = restored && *restored == source;
        std::printf("case %zu: raw %zu stored %zu %s\n", index, source.size(), stored.size(),
                    ok ? "ok" : "FAILED");
        if (!ok) {
            ++failures;
        }

        if (!stored.empty()) {
            std::vector<std::uint8_t> corrupt = stored;
            corrupt[corrupt.size() / 2] ^= 0x40;
            if (const auto decoded = euphoria::shader::decode(corrupt); decoded && *decoded != source) {
                std::printf("case %zu: corruption not detected\n", index);
                ++failures;
            }
        }
    }

    euphoria::shader::Cache cache{std::filesystem::temp_directory_path() / "euphoria-selftest"};
    const std::vector<std::uint8_t> payload{'d', 'x', 'i', 'l', 1, 2, 3, 4};
    const std::uint64_t key = euphoria::shader::Cache::key(payload);
    if (!cache.store(key, payload)) {
        std::printf("cache store FAILED\n");
        ++failures;
    } else {
        const auto loaded = cache.load(key);
        if (!loaded || *loaded != payload) {
            std::printf("cache load FAILED\n");
            ++failures;
        } else {
            std::printf("cache round trip ok\n");
        }
    }

    {
        const std::vector<std::uint8_t> dxil{0xBC, 0xC0, 0xDE, 0x35, 1, 2, 3};
        std::vector<std::uint8_t> sample(36 + 8 + dxil.size(), 0);
        const auto put32 = [&sample](std::size_t offset, std::uint32_t value) {
            for (int index = 0; index < 4; ++index) {
                sample[offset + static_cast<std::size_t>(index)] =
                    static_cast<std::uint8_t>((value >> (index * 8)) & 0xFF);
            }
        };
        put32(0, 0x43425844);
        put32(20, 1);
        put32(24, static_cast<std::uint32_t>(sample.size()));
        put32(28, 1);
        put32(32, 36);
        put32(36, 0x4C495844);
        put32(40, static_cast<std::uint32_t>(dxil.size()));
        std::memcpy(sample.data() + 44, dxil.data(), dxil.size());

        const auto parsed = euphoria::shader::parse_container(sample);
        const auto lifted = euphoria::shader::lift(sample);
        const bool ok = parsed && parsed->chunks.size() == 1 && parsed->chunks[0].name == "DXIL"
                     && lifted.ok && lifted.dxil == dxil;
        std::printf("container passthrough: %s\n", ok ? "ok" : "FAILED");
        if (!ok) {
            ++failures;
        }
    }

    std::printf("%s\n", failures == 0 ? "selftest passed" : "selftest FAILED");
    return failures == 0 ? 0 : 1;
}

int inspect(const char* path)
{
    const std::vector<std::uint8_t> data = read_file(path);
    if (data.empty()) {
        std::printf("cannot read %s\n", path);
        return 1;
    }

    const std::optional<euphoria::shader::Container> container =
        euphoria::shader::parse_container(data);
    if (!container) {
        std::printf("%s: %zu bytes, not a DXBC container\n", path, data.size());
        return 1;
    }

    std::printf("%s: %zu chunks\n", path, container->chunks.size());
    for (const auto& chunk : container->chunks) {
        std::printf("  %s %zu bytes\n", chunk.name.c_str(), chunk.data.size());
    }
    return 0;
}

int lift(const char* input_path, const char* output_path)
{
    const std::vector<std::uint8_t> data = read_file(input_path);
    if (data.empty()) {
        std::printf("cannot read %s\n", input_path);
        return 1;
    }

    const euphoria::shader::LiftResult result = euphoria::shader::lift(data);
    if (!result.ok) {
        std::printf("lift failed: %s\n", result.error.c_str());
        return 1;
    }
    if (!write_file(output_path, result.dxil)) {
        std::printf("cannot write %s\n", output_path);
        return 1;
    }
    std::printf("wrote %zu bytes to %s\n", result.dxil.size(), output_path);
    return 0;
}

int bench(const char* path)
{
    const std::vector<std::uint8_t> data = read_file(path);
    if (data.empty()) {
        std::printf("cannot read %s\n", path);
        return 1;
    }

    const auto start = std::chrono::steady_clock::now();
    const std::vector<std::uint8_t> stored = euphoria::shader::encode(data);
    const auto encoded = std::chrono::steady_clock::now();
    const auto restored = euphoria::shader::decode(stored);
    const auto decoded = std::chrono::steady_clock::now();

    if (!restored || *restored != data) {
        std::printf("round trip FAILED\n");
        return 1;
    }

    const auto seconds = [](auto from, auto to) {
        return std::chrono::duration<double>(to - from).count();
    };
    const double raw_mib = static_cast<double>(data.size()) / (1024.0 * 1024.0);
    const double encode_seconds = seconds(start, encoded);
    const double decode_seconds = seconds(encoded, decoded);

    std::printf("raw %.2f MiB -> %.2f MiB (%.1f%%)\n", raw_mib,
                static_cast<double>(stored.size()) / (1024.0 * 1024.0),
                100.0 * static_cast<double>(stored.size()) / static_cast<double>(data.size()));
    std::printf("encode %.1f MiB/s (%.3f s), decode %.1f MiB/s (%.3f s)\n",
                raw_mib / encode_seconds, encode_seconds,
                raw_mib / decode_seconds, decode_seconds);
    return 0;
}

int decode(const char* path)
{
    const std::vector<std::uint8_t> data = read_file(path);
    if (data.empty()) {
        std::printf("cannot read %s\n", path);
        return 1;
    }
    const std::optional<euphoria::shader::XboxShader> shader =
        euphoria::shader::extract_xbox_shader(data);
    if (!shader) {
        std::printf("extract failed\n");
        return 1;
    }
    std::printf("stage %s wave %u vgpr %u sgpr %u words %zu\n",
                euphoria::shader::stage_name(shader->stage), shader->wave, shader->vgprs,
                shader->sgprs, shader->code.size());
    std::vector<openscarlett::Rdna2Instruction> instructions;
    const openscarlett::Status status =
        openscarlett::DisassembleRdna2(shader->code, instructions);
    std::printf("status %d %s, decoded %zu\n", static_cast<int>(status.code),
                status.message.c_str(), instructions.size());
    const std::size_t show = std::min<std::size_t>(8, instructions.size());
    for (std::size_t index = 0; index < show; ++index) {
        std::printf("  %s\n", openscarlett::FormatRdna2Instruction(instructions[index]).c_str());
    }
    return status.ok() ? 0 : 1;
}

int lift_isa(const char* input_path, const char* output_path)
{
    const std::vector<std::uint8_t> data = read_file(input_path);
    const std::optional<euphoria::shader::XboxShader> shader =
        data.empty() ? std::nullopt : euphoria::shader::extract_xbox_shader(data);
    if (!shader) {
        std::printf("extract failed\n");
        return 1;
    }

    std::vector<openscarlett::Rdna2Instruction> instructions;
    const openscarlett::Status status =
        openscarlett::DisassembleRdna2(shader->code, instructions);
    if (!status.ok()) {
        std::printf("disassembly failed: %s\n", status.message.c_str());
        return 1;
    }

    const euphoria::shader::HlslLift hlsl = euphoria::shader::lift_to_hlsl(shader->stage, instructions);
    if (!hlsl.ok) {
        std::printf("hlsl failed: %s\n", hlsl.error.c_str());
        return 1;
    }

    const std::filesystem::path cache = std::filesystem::temp_directory_path() / "euphoria-dxc";
    const euphoria::shader::DxcResult compiled =
        euphoria::shader::compile_hlsl(hlsl.hlsl, hlsl.stage, cache);
    if (!compiled.ok) {
        std::printf("dxc failed: %s\n", compiled.diagnostics.c_str());
        return 1;
    }

    if (!write_file(output_path, compiled.dxil)) {
        std::printf("cannot write %s\n", output_path);
        return 1;
    }
    std::printf("stage %s, %zu instructions, DXIL %zu bytes\n",
                euphoria::shader::stage_name(shader->stage), instructions.size(),
                compiled.dxil.size());
    return 0;
}

int lift_all(const char* directory, const char* output_directory, std::size_t limit)
{
    const std::filesystem::path output{output_directory};
    std::error_code error;
    std::filesystem::create_directories(output, error);

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".cso") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    if (limit != 0 && files.size() > limit) {
        files.resize(limit);
    }

    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    std::atomic<std::size_t> next{0};
    std::mutex merge_mutex;
    std::uint64_t lifted = 0;
    std::uint64_t partial = 0;
    std::uint64_t stubbed = 0;
    std::uint64_t failed = 0;
    std::unordered_map<std::string, std::uint64_t> reasons;

    std::vector<std::thread> workers;
    workers.reserve(threads);
    for (unsigned worker = 0; worker < threads; ++worker) {
        workers.emplace_back([&] {
            std::uint64_t local_lifted = 0;
            std::uint64_t local_partial = 0;
            std::uint64_t local_stubbed = 0;
            std::uint64_t local_failed = 0;
            std::unordered_map<std::string, std::uint64_t> local_reasons;
            while (true) {
                const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= files.size()) {
                    break;
                }

                const std::filesystem::path path = output / (files[index].stem().string() + ".dxil");
                if (std::filesystem::is_regular_file(path, error)) {
                    ++local_lifted;
                    continue;
                }

                const std::vector<std::uint8_t> data = read_file(files[index].string().c_str());
                const std::optional<euphoria::shader::XboxShader> shader =
                    data.empty() ? std::nullopt : euphoria::shader::extract_xbox_shader(data);
                if (!shader) {
                    ++local_failed;
                    ++local_reasons["extract"];
                    continue;
                }

                std::vector<openscarlett::Rdna2Instruction> instructions;
                const openscarlett::Status status =
                    openscarlett::DisassembleRdna2(shader->code, instructions);
                if (!status.ok()) {
                    ++local_failed;
                    ++local_reasons["disassembly"];
                    continue;
                }

                const euphoria::shader::HlslLift hlsl =
                    euphoria::shader::lift_to_hlsl(shader->stage, instructions, true);
                if (!hlsl.ok) {
                    ++local_failed;
                    ++local_reasons[hlsl.error];
                    continue;
                }

                const std::filesystem::path cache = std::filesystem::temp_directory_path()
                                                  / "euphoria-dxc";
                const euphoria::shader::DxcResult compiled =
                    euphoria::shader::compile_hlsl(hlsl.hlsl, hlsl.stage, cache);
                if (!compiled.ok) {
                    ++local_failed;
                    std::string reason = compiled.diagnostics;
                    const std::size_t line_end = reason.find('\n');
                    if (line_end != std::string::npos) {
                        reason.resize(line_end);
                    }
                    ++local_reasons["dxc: " + reason];
                    continue;
                }

                if (!write_file(path.string().c_str(), compiled.dxil)) {
                    ++local_failed;
                    ++local_reasons["write"];
                    continue;
                }

                if (hlsl.stub) {
                    ++local_stubbed;
                } else if (hlsl.partial) {
                    ++local_partial;
                } else {
                    ++local_lifted;
                }
            }

            std::scoped_lock lock(merge_mutex);
            lifted += local_lifted;
            partial += local_partial;
            stubbed += local_stubbed;
            failed += local_failed;
            for (const auto& [reason, count] : local_reasons) {
                reasons[reason] += count;
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    std::printf("shaders %llu, lifted %llu, partial %llu, stubs %llu, failed %llu\n",
                static_cast<unsigned long long>(files.size()),
                static_cast<unsigned long long>(lifted),
                static_cast<unsigned long long>(partial),
                static_cast<unsigned long long>(stubbed),
                static_cast<unsigned long long>(failed));
    for (const auto& [reason, count] : reasons) {
        std::printf("  %-60s %llu\n", reason.c_str(), static_cast<unsigned long long>(count));
    }
    return 0;
}

int gpu_memory()
{
    euphoria::gpu::MemoryManager memory;
    if (!memory.initialize()) {
        std::printf("gpu memory: device creation failed\n");
        return 1;
    }

    std::vector<std::uint8_t> data(1 << 20);
    for (std::size_t index = 0; index < data.size(); ++index) {
        data[index] = static_cast<std::uint8_t>(index);
    }

    ID3D12Resource* device = memory.upload(data, euphoria::gpu::Placement::DeviceLocal);
    ID3D12Resource* unified = memory.upload(data, euphoria::gpu::Placement::Unified);
    if (device == nullptr || unified == nullptr) {
        std::printf("gpu memory: upload failed\n");
        return 1;
    }

    std::printf("gpu memory after upload: %s\n", memory.description().c_str());
    memory.release(device);
    memory.release(unified);
    std::printf("gpu memory after release: %s\n", memory.description().c_str());
    return 0;
}

int aot(const char* directory, const char* cache_directory, unsigned threads)
{
    const euphoria::shader::AotStats stats =
        euphoria::shader::run_aot(directory, cache_directory, threads);
    std::printf("shaders %llu, decoded %llu, cached %llu, failed %llu\n",
                static_cast<unsigned long long>(stats.shaders),
                static_cast<unsigned long long>(stats.decoded),
                static_cast<unsigned long long>(stats.cached),
                static_cast<unsigned long long>(stats.failed));
    std::printf("words %llu, instructions %llu, %.2f s (%.1f Mi instructions/s)\n",
                static_cast<unsigned long long>(stats.words),
                static_cast<unsigned long long>(stats.instructions), stats.seconds,
                stats.seconds > 0.0
                    ? static_cast<double>(stats.instructions) / stats.seconds / 1e6
                    : 0.0);
    std::printf("blocks %llu, branches %llu, returns %llu\n",
                static_cast<unsigned long long>(stats.blocks),
                static_cast<unsigned long long>(stats.branches),
                static_cast<unsigned long long>(stats.returns));
    for (const auto& entry : stats.formats) {
        std::printf("  %-28s %llu\n", entry.name.c_str(),
                    static_cast<unsigned long long>(entry.count));
    }
    std::printf("top opcodes\n");
    for (const auto& entry : stats.top) {
        std::printf("  %-28s %llu\n", entry.name.c_str(),
                    static_cast<unsigned long long>(entry.count));
    }
    return stats.failed == 0 ? 0 : 1;
}

int codec(const char* input_path, const char* output_path)
{    const std::vector<std::uint8_t> data = read_file(input_path);
    if (data.empty()) {
        std::printf("cannot read %s\n", input_path);
        return 1;
    }

    const std::vector<std::uint8_t> stored = euphoria::shader::encode(data);
    if (!write_file(output_path, stored)) {
        std::printf("cannot write %s\n", output_path);
        return 1;
    }
    std::printf("raw %zu -> stored %zu (%.1f%%)\n", data.size(), stored.size(),
                data.empty() ? 0.0 : 100.0 * static_cast<double>(stored.size()) / data.size());
    return 0;
}

int formats_selftest()
{
    std::string report;
    int failures = euphoria::formats::bcpack::self_test(report);
    failures += euphoria::formats::inflate_self_test(report);
    failures += euphoria::shim::dstorage::self_test(report);
    std::printf("%s%s\n", report.c_str(), failures == 0 ? "formats selftest passed" : "formats selftest FAILED");
    return failures == 0 ? 0 : 1;
}

int xmem_selftest()
{
    std::string report;
    const int failures = euphoria::shim::xmem::self_test(report);
    std::printf("%s%s\n", report.c_str(), failures == 0 ? "xmem selftest passed" : "xmem selftest FAILED");
    return failures == 0 ? 0 : 1;
}

}

namespace euphoria::shader::gfx10::selftest {

void register_dxbc_tests();
void register_dxil_tests();
void register_native_tests();
void register_bitcode_tests();
void register_bitcode_opt_tests();
void register_container_tests();
void register_shader_tests();
void register_structure_tests();
void register_spill_tests();

}

namespace {

int gfx10_selftest(const char* module)
{
    namespace selftest = euphoria::shader::gfx10::selftest;
    constexpr void (*kRegistrars[])() = {
        selftest::register_decode_tests,
        selftest::register_opcodes_tests,
        selftest::register_sp3_tests,
        selftest::register_ir_tests,
        selftest::register_dxbc_tests,
        selftest::register_dxil_tests,
        selftest::register_native_tests,
        selftest::register_bitcode_tests,
        selftest::register_bitcode_opt_tests,
        selftest::register_container_tests,
        selftest::register_shader_tests,
        selftest::register_structure_tests,
        selftest::register_spill_tests,
    };
    for (const auto registrar : kRegistrars) {
        registrar();
    }
    std::string report;
    const selftest::Summary summary = selftest::run(module != nullptr ? module : "", report);
    std::printf("%sgfx10 selftest: %zu passed, %zu failed\n", report.c_str(), summary.passed, summary.failed);
    return summary.failed == 0 && summary.passed != 0 ? 0 : 1;
}

int gfx10_disasm(const char* path)
{
    namespace gfx10 = euphoria::shader::gfx10;
    const std::vector<std::uint8_t> data = read_file(path);
    const std::optional<euphoria::shader::XboxShader> shader =
        data.empty() ? std::nullopt : euphoria::shader::extract_xbox_shader(data);
    if (!shader) {
        std::printf("extract failed\n");
        return 1;
    }
    std::vector<std::uint8_t> code(shader->code.size() * 4);
    std::memcpy(code.data(), shader->code.data(), code.size());
    const auto list = gfx10::decode(code);
    if (!list) {
        std::printf("decode failed: %s\n", list.error().message().c_str());
        return 1;
    }
    const bool wave64 = shader->wave == 64;
    std::printf("stage %s wave %u, %zu instructions\n", euphoria::shader::stage_name(shader->stage), shader->wave,
                list->size());
    for (const gfx10::Instruction& inst : *list) {
        std::string words;
        for (const std::uint32_t word : inst.words) {
            words += std::format("{:08X} ", word);
        }
        std::printf("%06zx  %-28s %s\n", inst.offset, words.c_str(), inst.text(wave64).c_str());
    }
    return 0;
}

int gfx10_coverage(const char* directory, std::size_t limit)
{
    namespace gfx10 = euphoria::shader::gfx10;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".cso") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    if (limit != 0 && files.size() > limit) {
        files.resize(limit);
    }

    std::uint64_t programs = 0;
    std::uint64_t extract_failed = 0;
    std::uint64_t decoded = 0;
    std::uint64_t instructions = 0;
    std::uint64_t unknown = 0;
    std::uint64_t built = 0;
    std::uint64_t text_bytes = 0;
    std::unordered_map<std::string, std::uint64_t> decode_errors;
    std::unordered_map<std::string, std::uint64_t> unknown_opcodes;
    std::unordered_map<std::string, std::uint64_t> build_errors;
    for (const auto& file : files) {
        const std::vector<std::uint8_t> data = read_file(file.string().c_str());
        const std::optional<euphoria::shader::XboxShader> shader =
            data.empty() ? std::nullopt : euphoria::shader::extract_xbox_shader(data);
        if (!shader) {
            ++extract_failed;
            continue;
        }
        ++programs;
        std::vector<std::uint8_t> code(shader->code.size() * 4);
        std::memcpy(code.data(), shader->code.data(), code.size());
        const auto list = gfx10::decode(code);
        if (!list) {
            ++decode_errors[list.error().detail];
            continue;
        }
        ++decoded;
        const bool wave64 = shader->wave == 64;
        for (const gfx10::Instruction& inst : *list) {
            ++instructions;
            if (!inst.name()) {
                ++unknown;
                ++unknown_opcodes[std::format("{} 0x{:03x}", gfx10::encoding_name(inst.encoding), inst.opcode)];
            }
            text_bytes += inst.text(wave64).size();
        }
        const auto program = gfx10::ir::build(code, wave64, 256);
        if (program) {
            ++built;
        } else {
            ++build_errors[program.error().message().substr(program.error().message().find(':') + 1)];
        }
    }

    const auto print = [](const char* title, const std::unordered_map<std::string, std::uint64_t>& counts) {
        std::vector<std::pair<std::string, std::uint64_t>> sorted(counts.begin(), counts.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (!sorted.empty()) {
            std::printf("%s\n", title);
        }
        for (std::size_t index = 0; index < sorted.size() && index < 40; ++index) {
            std::printf("  %-56s %llu\n", sorted[index].first.c_str(),
                        static_cast<unsigned long long>(sorted[index].second));
        }
    };
    std::printf("files %zu, programs %llu, extract failed %llu, decoded %llu\n", files.size(),
                static_cast<unsigned long long>(programs), static_cast<unsigned long long>(extract_failed),
                static_cast<unsigned long long>(decoded));
    std::printf("instructions %llu, unknown opcodes %llu (%.4f%%), sp3 text %llu bytes, ir built %llu\n",
                static_cast<unsigned long long>(instructions), static_cast<unsigned long long>(unknown),
                instructions != 0 ? 100.0 * static_cast<double>(unknown) / static_cast<double>(instructions) : 0.0,
                static_cast<unsigned long long>(text_bytes), static_cast<unsigned long long>(built));
    print("decode errors", decode_errors);
    print("unknown opcodes", unknown_opcodes);
    print("ir build errors", build_errors);
    return decoded == programs && unknown == 0 ? 0 : 1;
}

int gfx10_structure(const char* directory, std::size_t limit)
{
    namespace gfx10 = euphoria::shader::gfx10;
    std::vector<std::pair<std::string, std::vector<std::uint8_t>>> blobs;
    if (std::filesystem::is_regular_file(directory)) {
        const std::vector<std::uint8_t> image = read_file(directory);
        for (std::size_t at = 0; at + 32 <= image.size(); ++at) {
            if (std::memcmp(image.data() + at, "DXBC", 4) != 0) {
                continue;
            }
            std::uint32_t size = 0;
            std::memcpy(&size, image.data() + at + 24, 4);
            if (size < 32 || size > image.size() - at) {
                continue;
            }
            blobs.emplace_back(std::format("{}@{:#x}", std::filesystem::path(directory).filename().string(), at),
                               std::vector<std::uint8_t>(image.begin() + static_cast<std::ptrdiff_t>(at),
                                                         image.begin() + static_cast<std::ptrdiff_t>(at + size)));
            at += size - 1;
        }
    } else {
        std::vector<std::filesystem::path> paths;
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            if (entry.is_regular_file() && entry.path().extension() == ".cso") {
                paths.push_back(entry.path());
            }
        }
        std::sort(paths.begin(), paths.end());
        for (const auto& path : paths) {
            blobs.emplace_back(path.filename().string(), read_file(path.string().c_str()));
        }
    }
    if (limit != 0 && blobs.size() > limit) {
        blobs.resize(limit);
    }

    std::uint64_t containers = 0;
    std::uint64_t programs = 0;
    std::uint64_t built = 0;
    std::uint64_t structured = 0;
    std::uint64_t spilled = 0;
    std::uint64_t replaced = 0;
    std::uint64_t kept_loads = 0;
    std::uint64_t blocks = 0;
    std::uint64_t guards = 0;
    std::uint64_t lane_branches = 0;
    std::uint64_t uniform_branches = 0;
    std::uint64_t phi_masks = 0;
    std::size_t largest = 0;
    std::string largest_file;
    double slowest = 0.0;
    std::string slowest_file;
    std::unordered_map<std::string, std::uint64_t> container_errors;
    std::unordered_map<std::string, std::uint64_t> build_errors;
    std::unordered_map<std::string, std::uint64_t> structure_errors;
    std::unordered_map<std::string, std::uint64_t> failing_files;
    const auto started = std::chrono::steady_clock::now();
    for (const auto& [name, data] : blobs) {
        const auto container = gfx10::Container::parse(data);
        if (!container) {
            ++container_errors[container.error().message()];
            continue;
        }
        const gfx10::Part* part = container->part(gfx10::FourCC::DXIL);
        if (part == nullptr) {
            ++container_errors["no DXIL part"];
            continue;
        }
        const auto natives = gfx10::native_programs(part->data);
        if (!natives) {
            ++container_errors[natives.error().message()];
            continue;
        }
        ++containers;
        for (const gfx10::NativeProgram& native : *natives) {
            ++programs;
            auto program = gfx10::ir::build(native.patched_code(), native.wave64, native.vgprs);
            if (!program) {
                ++build_errors[program.error().message()];
                continue;
            }
            ++built;
            if (gfx10::spill::is_spilled(native)) {
                ++spilled;
                replaced += gfx10::spill::user_data(*program, native);
                for (const auto& block : program->blocks) {
                    for (const auto& inst : block.insts) {
                        kept_loads += inst.name().starts_with("s_load_") ? 1 : 0;
                    }
                }
            }
            const auto before = std::chrono::steady_clock::now();
            const auto st = gfx10::structure::structure(*program);
            const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - before).count();
            if (seconds > slowest) {
                slowest = seconds;
                slowest_file = name;
            }
            if (!st) {
                ++structure_errors[st.error().message()];
                ++failing_files[name];
                continue;
            }
            ++structured;
            blocks += program->blocks.size();
            for (const auto& row : st->guards) {
                for (const auto& guard : row) {
                    guards += guard.has_value() ? 1 : 0;
                }
            }
            for (const auto& exit : st->exits) {
                if (exit.kind != gfx10::structure::LaneExit::Kind::Branch) {
                    continue;
                }
                const auto support = st->bools.support(exit.when);
                const bool uniform = std::all_of(support.begin(), support.end(), [](const auto& atom) {
                    return atom.kind == gfx10::structure::Atom::Kind::Uniform;
                });
                ++(uniform ? uniform_branches : lane_branches);
            }
            phi_masks += st->phi_masks.size();
            if (st->bools.size() > largest) {
                largest = st->bools.size();
                largest_file = name;
            }
        }
    }
    const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    const auto print = [](const char* title, const std::unordered_map<std::string, std::uint64_t>& counts) {
        std::vector<std::pair<std::string, std::uint64_t>> sorted(counts.begin(), counts.end());
        std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        if (!sorted.empty()) {
            std::printf("%s\n", title);
        }
        for (std::size_t index = 0; index < sorted.size() && index < 40; ++index) {
            std::printf("  %-64s %llu\n", sorted[index].first.c_str(),
                        static_cast<unsigned long long>(sorted[index].second));
        }
    };
    const auto u = [](std::uint64_t n) { return static_cast<unsigned long long>(n); };
    std::printf("inputs %zu, containers %llu, programs %llu, ir built %llu, structured %llu\n", blobs.size(),
                u(containers), u(programs), u(built), u(structured));
    std::printf("spilled programs %llu, table loads replaced %llu, scalar loads left in them %llu\n", u(spilled),
                u(replaced), u(kept_loads));
    std::printf("blocks %llu, guarded instructions %llu, lane branches %llu, uniform branches %llu, phi masks %llu\n",
                u(blocks), u(guards), u(lane_branches), u(uniform_branches), u(phi_masks));
    std::printf("largest bools %zu nodes (%s), slowest %.3f s (%s), total %.1f s\n", largest, largest_file.c_str(),
                slowest, slowest_file.c_str(), total);
    print("container errors", container_errors);
    print("ir build errors", build_errors);
    print("structure errors", structure_errors);
    print("failing files", failing_files);
    return built == structured ? 0 : 1;
}

}

int main(int argc, char** argv)
{
    if (argc < 2) {
        return 1;
    }

    const std::string command = argv[1];
    if (command == "selftest") {
        return selftest();
    }
    if (command == "inspect" && argc >= 3) {
        return inspect(argv[2]);
    }
    if (command == "lift" && argc >= 4) {
        return lift(argv[2], argv[3]);
    }
    if (command == "codec" && argc >= 4) {
        return codec(argv[2], argv[3]);
    }
    if (command == "bench" && argc >= 3) {
        return bench(argv[2]);
    }
    if (command == "aot" && argc >= 4) {
        const unsigned threads = argc >= 5 ? static_cast<unsigned>(std::atoi(argv[4])) : 0;
        return aot(argv[2], argv[3], threads);
    }
    if (command == "decode" && argc >= 3) {
        return decode(argv[2]);
    }
    if (command == "lift-isa" && argc >= 4) {
        return lift_isa(argv[2], argv[3]);
    }
    if (command == "lift-all" && argc >= 4) {
        const std::size_t limit = argc >= 5 ? static_cast<std::size_t>(std::atoi(argv[4])) : 0;
        return lift_all(argv[2], argv[3], limit);
    }
    if (command == "formats-selftest") {
        return formats_selftest();
    }
    if (command == "xmem-selftest") {
        return xmem_selftest();
    }
    if (command == "gfx10-selftest") {
        return gfx10_selftest(argc >= 3 ? argv[2] : nullptr);
    }
    if (command == "gfx10-disasm" && argc >= 3) {
        return gfx10_disasm(argv[2]);
    }
    if (command == "gfx10-coverage" && argc >= 3) {
        const std::size_t limit = argc >= 4 ? static_cast<std::size_t>(std::atoi(argv[3])) : 0;
        return gfx10_coverage(argv[2], limit);
    }
    if (command == "gfx10-structure" && argc >= 3) {
        const std::size_t limit = argc >= 4 ? static_cast<std::size_t>(std::atoi(argv[3])) : 0;
        return gfx10_structure(argv[2], limit);
    }
    if (command == "gpu-memory") {
        return gpu_memory();
    }

    return 1;
}
