#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace euphoria::shader {

struct OpcodeCount {
    std::string name;
    std::uint64_t count{};
};

struct AotStats {
    std::uint64_t shaders{};
    std::uint64_t cached{};
    std::uint64_t decoded{};
    std::uint64_t failed{};
    std::uint64_t words{};
    std::uint64_t instructions{};
    std::uint64_t blocks{};
    std::uint64_t branches{};
    std::uint64_t returns{};
    double seconds{};
    std::vector<OpcodeCount> formats;
    std::vector<OpcodeCount> top;
};

AotStats run_aot(const std::filesystem::path& shader_directory,
                 const std::filesystem::path& cache_directory,
                 unsigned threads);

}
