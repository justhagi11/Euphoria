#include <euphoria/shim/gdk.hpp>

#include <cstring>

namespace euphoria::shim::gdk {
namespace {

using This = const Object*;

constexpr std::uint8_t kSilence[44]{
    'R', 'I', 'F', 'F', 0x24, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
    0x10, 0, 0, 0, 0x01, 0, 0x01, 0, 0x80, 0x3e, 0, 0, 0x00, 0x7d, 0, 0,
    0x02, 0, 0x10, 0, 'd', 'a', 't', 'a', 0, 0, 0, 0,
};

char g_synthesizer = 0;
char g_stream = 0;

HResult enumerate_voices(This, void*, const void*)
{
    log_first("XSpeechSynthesizerEnumerateInstalledVoices");
    return kOk;
}

HResult create(This, const void** out)
{
    log_first("XSpeechSynthesizerCreate");
    return put(out, static_cast<const void*>(&g_synthesizer));
}

HResult close(This, const void*)
{
    return kOk;
}

HResult set_default_voice(This, const void*)
{
    return kOk;
}

HResult set_custom_voice(This, const void*, const char*)
{
    return kOk;
}

HResult create_stream(This, const void*, const char*, const void** out)
{
    log_first("XSpeechSynthesizerCreateStreamFromText (silent)");
    return put(out, static_cast<const void*>(&g_stream));
}

HResult stream_size(This, const void*, std::size_t* out)
{
    return put(out, sizeof(kSilence));
}

HResult stream_data(This, const void*, std::size_t size, std::uint8_t* buffer, std::size_t* used)
{
    if (buffer == nullptr || size < sizeof(kSilence)) {
        return kNotSufficientBuffer;
    }
    std::memcpy(buffer, kSilence, sizeof(kSilence));
    if (used != nullptr) {
        *used = sizeof(kSilence);
    }
    return kOk;
}

const Table<22> kTable = [] {
    auto t = table<22>();
    set(t, 0x40, &enumerate_voices);
    set(t, 0x48, &create);
    set(t, 0x50, &close);
    set(t, 0x58, &set_default_voice);
    set(t, 0x60, &set_custom_voice);
    set(t, 0x68, &create_stream);
    set(t, 0x70, &close);
    set(t, 0x78, &stream_size);
    set(t, 0x80, &stream_data);
    set(t, 0xa8, &create_stream);
    return t;
}();

const Object kObject{kTable.data(), "accessibility"};

constexpr Guid kProvider{0x3e241977, 0x6237, 0x41e9, {0x85, 0x59, 0x10, 0x2c, 0x2d, 0x99, 0x83, 0xf1}};
constexpr Guid kIid{0xd722b373, 0x8c4d, 0x4692, {0x9e, 0x51, 0xd6, 0xad, 0x9b, 0x37, 0xaa, 0x7d}};

const std::array<Interface, 1> kInterfaces{{
    {kProvider, kIid, &kObject},
}};

}

std::span<const Interface> speech_interfaces()
{
    return kInterfaces;
}

}
