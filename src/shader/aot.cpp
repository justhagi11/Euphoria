#include <euphoria/shader/aot.hpp>

#include <euphoria/shader/cache.hpp>
#include <euphoria/shader/rdna2.hpp>
#include <euphoria/shader/xbox.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace euphoria::shader {
namespace {

constexpr std::uint32_t kManifestMagic = 0x32444145;

struct WorkerStats {
    std::uint64_t shaders{};
    std::uint64_t cached{};
    std::uint64_t decoded{};
    std::uint64_t failed{};
    std::uint64_t words{};
    std::uint64_t instructions{};
    std::uint64_t blocks{};
    std::uint64_t branches{};
    std::uint64_t returns{};
    std::array<std::uint64_t, kEncodingCount> formats{};
    std::unordered_map<std::uint32_t, std::uint64_t> opcodes;
};

std::vector<std::uint8_t> read_file(const std::filesystem::path& path)
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

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value)
{
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFF));
    }
}

std::vector<std::uint8_t> encode_manifest(const XboxShader& shader, const ScanResult& scan)
{
    std::vector<std::uint8_t> out;
    out.reserve(32 + kEncodingCount * 4 + scan.opcodes.size() * 8);
    put_u32(out, kManifestMagic);
    out.push_back(shader.stage);
    out.push_back(static_cast<std::uint8_t>(shader.wave));
    put_u16(out, static_cast<std::uint16_t>(shader.vgprs));
    put_u16(out, static_cast<std::uint16_t>(shader.sgprs));
    put_u16(out, 0);
    put_u32(out, static_cast<std::uint32_t>(scan.instructions));
    put_u32(out, static_cast<std::uint32_t>(scan.blocks));
    put_u32(out, static_cast<std::uint32_t>(scan.branches));
    put_u32(out, static_cast<std::uint32_t>(scan.returns));
    for (const std::uint64_t count : scan.formats) {
        put_u32(out, static_cast<std::uint32_t>(count));
    }
    put_u32(out, static_cast<std::uint32_t>(scan.opcodes.size()));
    for (const auto& [key, count] : scan.opcodes) {
        put_u32(out, key);
        put_u32(out, static_cast<std::uint32_t>(count));
    }
    return out;
}

void merge(WorkerStats& into, const WorkerStats& from)
{
    into.shaders += from.shaders;
    into.cached += from.cached;
    into.decoded += from.decoded;
    into.failed += from.failed;
    into.words += from.words;
    into.instructions += from.instructions;
    into.blocks += from.blocks;
    into.branches += from.branches;
    into.returns += from.returns;
    for (std::size_t index = 0; index < kEncodingCount; ++index) {
        into.formats[index] += from.formats[index];
    }
    for (const auto& [key, count] : from.opcodes) {
        into.opcodes[key] += count;
    }
}

std::string opcode_name(std::uint32_t key)
{
    char text[32]{};
    std::snprintf(text, sizeof(text), "%s:%02x", encoding_name(static_cast<Encoding>(key >> 16)),
                  key & 0xFFFF);
    return text;
}

}

AotStats run_aot(const std::filesystem::path& shader_directory,
                 const std::filesystem::path& cache_directory,
                 unsigned threads)
{
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(shader_directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".cso") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());

    const unsigned count = threads == 0
                             ? std::max(1u, std::thread::hardware_concurrency())
                             : threads;
    const Cache cache{cache_directory};

    std::atomic<std::size_t> next{0};
    std::mutex merge_mutex;
    WorkerStats total;
    const auto started = std::chrono::steady_clock::now();

    std::vector<std::thread> workers;
    workers.reserve(count);
    for (unsigned worker = 0; worker < count; ++worker) {
        workers.emplace_back([&] {
            WorkerStats local;
            while (true) {
                const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= files.size()) {
                    break;
                }

                ++local.shaders;
                const std::vector<std::uint8_t> file = read_file(files[index]);
                if (file.empty()) {
                    ++local.failed;
                    continue;
                }

                const std::uint64_t key = Cache::key(file);
                if (cache.load(key)) {
                    ++local.cached;
                    continue;
                }

                const std::optional<XboxShader> shader = extract_xbox_shader(file);
                if (!shader) {
                    ++local.failed;
                    continue;
                }

                const ScanResult scan = scan_rdna2(shader->code);
                local.words += scan.words;
                local.instructions += scan.instructions;
                local.blocks += scan.blocks;
                local.branches += scan.branches;
                local.returns += scan.returns;
                for (std::size_t format = 0; format < kEncodingCount; ++format) {
                    local.formats[format] += scan.formats[format];
                }
                for (const auto& [opcode, occurrences] : scan.opcodes) {
                    local.opcodes[opcode] += occurrences;
                }

                cache.store(key, encode_manifest(*shader, scan));
                ++local.decoded;
            }

            std::scoped_lock lock(merge_mutex);
            merge(total, local);
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    AotStats result;
    result.shaders = total.shaders;
    result.cached = total.cached;
    result.decoded = total.decoded;
    result.failed = total.failed;
    result.words = total.words;
    result.instructions = total.instructions;
    result.blocks = total.blocks;
    result.branches = total.branches;
    result.returns = total.returns;
    result.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    for (std::size_t format = 0; format < kEncodingCount; ++format) {
        if (total.formats[format] != 0) {
            result.formats.push_back({encoding_name(static_cast<Encoding>(format)),
                                      total.formats[format]});
        }
    }

    std::vector<std::pair<std::uint32_t, std::uint64_t>> ordered(total.opcodes.begin(),
                                                                 total.opcodes.end());
    std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
        return left.second > right.second;
    });
    const std::size_t keep = std::min<std::size_t>(24, ordered.size());
    result.top.reserve(keep);
    for (std::size_t index = 0; index < keep; ++index) {
        result.top.push_back({opcode_name(ordered[index].first), ordered[index].second});
    }
    return result;
}

}
