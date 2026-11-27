#pragma once

#include <cstddef>
#include <cstdint>
#include <source_location>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace euphoria::shader::gfx10::selftest {

class Context {
public:
    bool expect(bool condition, std::string_view what,
                std::source_location location = std::source_location::current());
    bool expect_eq(std::string_view actual, std::string_view expected,
                   std::source_location location = std::source_location::current());

    std::size_t failures() const noexcept { return failures_; }
    const std::string& log() const noexcept { return log_; }

private:
    std::size_t failures_{0};
    std::string log_;
};

using Case = void (*)(Context&);

void register_case(std::string_view module, std::string_view name, Case test);

struct Summary {
    std::size_t passed{0};
    std::size_t failed{0};
};

Summary run(std::string_view module, std::string& report);

std::vector<std::string> modules();

std::vector<std::uint8_t> bytes(std::span<const std::uint32_t> words);

void register_decode_tests();
void register_opcodes_tests();
void register_sp3_tests();
void register_ir_tests();

}
