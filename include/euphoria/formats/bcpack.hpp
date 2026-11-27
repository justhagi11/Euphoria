#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace euphoria::formats::bcpack {

inline constexpr std::size_t tables_size = 77982;

struct Tables;

struct TablesDelete {
    void operator()(Tables* tables) const noexcept;
};

using TablesPtr = std::unique_ptr<Tables, TablesDelete>;

std::span<const std::uint8_t> builtin_table_bytes() noexcept;
std::expected<TablesPtr, std::string_view> parse_tables(std::span<const std::uint8_t> bytes);
const Tables* builtin_tables() noexcept;
std::string_view builtin_tables_error() noexcept;

std::expected<void, std::string_view> decode(std::uint8_t bc, std::span<const std::uint8_t> source,
                                             std::span<std::uint8_t> destination,
                                             const Tables& tables);

int self_test(std::string& report);

}
