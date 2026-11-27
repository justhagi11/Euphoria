#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace euphoria::shader {

struct DxcResult {
    bool ok{};
    std::vector<std::uint8_t> dxil;
    std::string diagnostics;
};

DxcResult compile_hlsl(const std::string& hlsl, std::uint8_t stage,
                       const std::filesystem::path& work_directory);

std::filesystem::path find_dxc();

}
