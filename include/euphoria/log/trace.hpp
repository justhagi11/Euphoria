#pragma once

#include <cstdint>

namespace euphoria::trace {

void install_exit_hook(std::uintptr_t guest_base, std::uint32_t guest_size);

}
