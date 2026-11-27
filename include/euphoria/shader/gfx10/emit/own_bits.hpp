#pragma once

#include <euphoria/shader/gfx10/ir.hpp>
#include <euphoria/shader/gfx10/structure.hpp>

#include <unordered_set>

namespace euphoria::shader::gfx10::emit::own_bits {

struct OwnBits {
    std::unordered_set<ir::Value> totals;
    std::unordered_set<ir::Value> alike;
};

OwnBits find(const ir::Program& p, const structure::Structured& st, bool why);

}
