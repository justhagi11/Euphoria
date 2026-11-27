#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace euphoria::relinker {

using Address = std::uintptr_t;

struct Symbol {
    std::string_view module;
    std::string_view name;
    std::uint32_t ordinal;
    Address address;
};

class SymbolTable {
public:
    void add(Symbol symbol);
    std::optional<Address> find(std::string_view module, std::string_view name) const;
    std::optional<Address> find(std::string_view module, std::uint32_t ordinal) const;
    std::span<const Symbol> symbols() const noexcept;

private:
    std::vector<Symbol> symbols_;
};

}
