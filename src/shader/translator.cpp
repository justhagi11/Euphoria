#include <euphoria/shader/translator.hpp>

#include <euphoria/shader/container.hpp>

#include <algorithm>
#include <string_view>

namespace euphoria::shader {

LiftResult lift(std::span<const std::uint8_t> console_shader)
{
    LiftResult result;
    std::optional<Container> container = parse_container(console_shader);
    if (!container) {
        result.error = "not a shader container";
        return result;
    }

    result.chunks = container->chunks.size();

    const Chunk* dxil = container->find("DXIL");
    if (dxil == nullptr) {
        result.error = "no DXIL part";
        return result;
    }

    const auto contains = [dxil](std::string_view tag) {
        if (dxil->data.size() < tag.size()) {
            return false;
        }
        const auto* begin = reinterpret_cast<const char*>(dxil->data.data());
        return std::search(begin, begin + dxil->data.size(), tag.begin(), tag.end()) != begin + dxil->data.size();
    };

    if (contains("XBCH") || contains("XDAT")) {
        result.error = "native RDNA2 program (XBCH/FSSM); ISA lifting required";
        return result;
    }

    for (std::size_t index = 0; index + 4 <= dxil->data.size(); ++index) {
        if (dxil->data[index] == 0xBC && dxil->data[index + 1] == 0xC0
            && dxil->data[index + 2] == 0xDE && dxil->data[index + 3] == 0x35) {
            result.ok = true;
            result.dxil.assign(dxil->data.begin(), dxil->data.end());
            return result;
        }
    }

    result.error = "DXIL part has no bitcode";
    return result;
}

}
