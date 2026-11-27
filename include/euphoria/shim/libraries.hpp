#pragma once

#include <euphoria/shim/registry.hpp>

namespace euphoria::shim {

const Library& xapi_library() noexcept;
const Library& gdk_library() noexcept;
const Library& kernel_library() noexcept;
const Library& dx_library() noexcept;
const Library& audio_library() noexcept;
const Library& input_library() noexcept;
const Library& net_library() noexcept;
const Library& live_library() noexcept;
const Library& system_library() noexcept;
const Library& xg_library() noexcept;
const Library& acphal_library() noexcept;

}
