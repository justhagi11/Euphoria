#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

namespace euphoria::formats {

std::expected<std::size_t, std::string_view> zlib_inflate(std::span<const std::uint8_t> source,
                                                          std::span<std::uint8_t> destination);

int inflate_self_test(std::string& report);

}
