#include <euphoria/log/log.hpp>
#include <euphoria/shim/libraries.hpp>

#include <array>

namespace euphoria::shim {
namespace {

std::uintptr_t stub(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t)
{
    log::warn("xaudio2: unimplemented shim called");
    return 0;
}

constexpr std::array<Entry, 1> kEntries{{
    {"XAudio2Create", 0, stub},
}};

const Library kLibrary{"xaudio2", kEntries};

}

const Library& audio_library() noexcept
{
    return kLibrary;
}

}
