#include <euphoria/relinker/symbol_table.hpp>

namespace euphoria::relinker {

void SymbolTable::add(Symbol symbol)
{
    symbols_.push_back(symbol);
}

std::optional<Address> SymbolTable::find(std::string_view module, std::string_view name) const
{
    for (const auto& symbol : symbols_) {
        if (symbol.module == module && symbol.name == name) {
            return symbol.address;
        }
    }
    return std::nullopt;
}

std::optional<Address> SymbolTable::find(std::string_view module, std::uint32_t ordinal) const
{
    for (const auto& symbol : symbols_) {
        if (symbol.module == module && symbol.ordinal == ordinal) {
            return symbol.address;
        }
    }
    return std::nullopt;
}

std::span<const Symbol> SymbolTable::symbols() const noexcept
{
    return symbols_;
}

}
