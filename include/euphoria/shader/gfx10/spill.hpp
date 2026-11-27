#pragma once

#include <euphoria/shader/gfx10/error.hpp>
#include <euphoria/shader/gfx10/ir.hpp>
#include <euphoria/shader/gfx10/native.hpp>

#include <cstddef>
#include <expected>

namespace euphoria::shader::gfx10::spill {

std::expected<ir::Program, ShaderError> build(const native::NativeProgram& program);
bool is_spilled(const native::NativeProgram& program);
std::size_t user_data(ir::Program& program, const native::NativeProgram& source);

}
