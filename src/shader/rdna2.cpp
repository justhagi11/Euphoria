#include <euphoria/shader/rdna2.hpp>

#include <algorithm>
#include <unordered_map>

namespace euphoria::shader {
namespace {

constexpr std::uint32_t kLiteral = 255;

struct Decoded {
    Encoding encoding{Encoding::Unknown};
    std::uint16_t opcode{};
    std::uint32_t size{1};
};

Decoded decode(std::span<const std::uint32_t> words, std::size_t index)
{
    const std::uint32_t w0 = words[index];
    Decoded result;

    if ((w0 >> 23) == 0x17Du) {
        result.encoding = Encoding::Sop1;
        result.opcode = static_cast<std::uint16_t>((w0 >> 8) & 0xFF);
        result.size = (w0 & 0xFF) == kLiteral ? 2 : 1;
    } else if ((w0 >> 23) == 0x17Eu) {
        result.encoding = Encoding::Sopc;
        result.opcode = static_cast<std::uint16_t>((w0 >> 16) & 0x7F);
        result.size = ((w0 & 0xFF) == kLiteral || ((w0 >> 8) & 0xFF) == kLiteral) ? 2 : 1;
    } else if ((w0 >> 23) == 0x17Fu) {
        result.encoding = Encoding::Sopp;
        result.opcode = static_cast<std::uint16_t>((w0 >> 16) & 0x7F);
    } else if ((w0 >> 28) == 0xBu) {
        result.encoding = Encoding::Sopk;
        result.opcode = static_cast<std::uint16_t>((w0 >> 23) & 0x1F);
        result.size = (result.opcode == 0x14 || result.opcode == 0x15) ? 2 : 1;
    } else if ((w0 >> 30) == 0x2u) {
        result.encoding = Encoding::Sop2;
        result.opcode = static_cast<std::uint16_t>((w0 >> 23) & 0x7F);
        result.size = ((w0 & 0xFF) == kLiteral || ((w0 >> 8) & 0xFF) == kLiteral) ? 2 : 1;
    } else if ((w0 >> 25) == 0x3Fu) {
        result.encoding = Encoding::Vop1;
        result.opcode = static_cast<std::uint16_t>((w0 >> 9) & 0xFF);
        const std::uint32_t src0 = w0 & 0x1FF;
        result.size = (src0 == kLiteral || src0 == 0xE9 || src0 == 0xEA
                       || src0 == 0xF9 || src0 == 0xFA)
                        ? 2
                        : 1;
    } else if ((w0 >> 25) == 0x3Eu) {
        result.encoding = Encoding::Vopc;
        result.opcode = static_cast<std::uint16_t>((w0 >> 17) & 0xFF);
        const std::uint32_t src0 = w0 & 0x1FF;
        result.size = (src0 == kLiteral || src0 == 0xE9 || src0 == 0xEA
                       || src0 == 0xF9 || src0 == 0xFA)
                        ? 2
                        : 1;
    } else if ((w0 >> 31) == 0x0u) {
        result.encoding = Encoding::Vop2;
        result.opcode = static_cast<std::uint16_t>((w0 >> 25) & 0x3F);
        const std::uint32_t src0 = w0 & 0x1FF;
        result.size = (src0 == kLiteral || src0 == 0xE9 || src0 == 0xEA
                       || src0 == 0xF9 || src0 == 0xFA)
                        ? 2
                        : 1;
    } else if ((w0 >> 26) == 0x32u) {
        result.encoding = Encoding::Vintrp;
        result.opcode = static_cast<std::uint16_t>((w0 >> 16) & 0x3);
    } else if ((w0 >> 26) == 0x33u) {
        result.encoding = Encoding::Vop3p;
        result.opcode = static_cast<std::uint16_t>((w0 >> 16) & 0x7F);
        result.size = 2;
        if (index + 1 < words.size()) {
            const std::uint32_t w1 = words[index + 1];
            if ((w1 & 0x1FF) == kLiteral || ((w1 >> 9) & 0x1FF) == kLiteral
                || ((w1 >> 18) & 0x1FF) == kLiteral) {
                result.size = 3;
            }
        }
    } else if ((w0 >> 26) == 0x34u || (w0 >> 26) == 0x35u || (w0 >> 26) == 0x36u) {
        if ((w0 >> 26) == 0x36u) {
            result.encoding = Encoding::Ds;
            result.opcode = static_cast<std::uint16_t>((w0 >> 18) & 0xFF);
            result.size = 2;
        } else {
            result.encoding = Encoding::Vop3;
            result.opcode = static_cast<std::uint16_t>((w0 >> 16) & 0x3FF);
            result.size = 2;
            if (index + 1 < words.size()) {
                const std::uint32_t w1 = words[index + 1];
                if ((w1 & 0x1FF) == kLiteral || ((w1 >> 9) & 0x1FF) == kLiteral
                    || ((w1 >> 18) & 0x1FF) == kLiteral) {
                    result.size = 3;
                }
            }
        }
    } else if ((w0 >> 26) == 0x37u) {
        result.encoding = Encoding::Flat;
        result.opcode = static_cast<std::uint16_t>((w0 >> 18) & 0xFF);
        result.size = 2;
    } else if ((w0 >> 26) == 0x38u) {
        result.encoding = Encoding::Mubuf;
        result.opcode = static_cast<std::uint16_t>((w0 >> 18) & 0xFF);
        result.size = 2;
    } else if ((w0 >> 26) == 0x3Au) {
        result.encoding = Encoding::Mtbuf;
        result.opcode = static_cast<std::uint16_t>((w0 >> 18) & 0xFF);
        result.size = 2;
    } else if ((w0 >> 26) == 0x3Cu) {
        result.encoding = Encoding::Mimg;
        result.opcode = static_cast<std::uint16_t>((w0 >> 18) & 0xFF);
        result.size = 2;
    } else if ((w0 >> 26) == 0x3Du) {
        result.encoding = Encoding::Smem;
        result.opcode = static_cast<std::uint16_t>((w0 >> 18) & 0xFF);
        result.size = 2;
    } else if ((w0 >> 26) == 0x3Eu) {
        result.encoding = Encoding::Exp;
        result.size = 2;
    }

    if (index + result.size > words.size()) {
        result.size = 1;
        result.encoding = Encoding::Unknown;
    }
    return result;
}

}

const char* encoding_name(Encoding encoding) noexcept
{
    switch (encoding) {
    case Encoding::Sop1: return "SOP1";
    case Encoding::Sop2: return "SOP2";
    case Encoding::Sopc: return "SOPC";
    case Encoding::Sopk: return "SOPK";
    case Encoding::Sopp: return "SOPP";
    case Encoding::Vop1: return "VOP1";
    case Encoding::Vop2: return "VOP2";
    case Encoding::Vopc: return "VOPC";
    case Encoding::Vop3: return "VOP3";
    case Encoding::Vop3p: return "VOP3P";
    case Encoding::Vintrp: return "VINTRP";
    case Encoding::Exp: return "EXP";
    case Encoding::Ds: return "DS";
    case Encoding::Smem: return "SMEM";
    case Encoding::Mubuf: return "MUBUF";
    case Encoding::Mtbuf: return "MTBUF";
    case Encoding::Mimg: return "MIMG";
    case Encoding::Flat: return "FLAT";
    case Encoding::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

ScanResult scan_rdna2(std::span<const std::uint32_t> words)
{
    ScanResult result;
    result.words = words.size();

    std::unordered_map<std::uint32_t, std::uint64_t> opcodes;
    std::size_t index = 0;
    if (!words.empty()) {
        result.blocks = 1;
    }
    while (index < words.size()) {
        const Decoded decoded = decode(words, index);
        ++result.instructions;
        ++result.formats[static_cast<std::size_t>(decoded.encoding)];
        ++opcodes[(static_cast<std::uint32_t>(decoded.encoding) << 16) | decoded.opcode];
        if (decoded.encoding == Encoding::Sopp) {
            if (decoded.opcode == 0x01) {
                ++result.returns;
            } else if (decoded.opcode >= 0x02 && decoded.opcode <= 0x0F) {
                ++result.branches;
            }
            if ((decoded.opcode == 0x01 || (decoded.opcode >= 0x02 && decoded.opcode <= 0x0F))
                && index + decoded.size < words.size()) {
                ++result.blocks;
            }
        }
        index += decoded.size;
    }

    result.opcodes.reserve(opcodes.size());
    for (const auto& [key, count] : opcodes) {
        result.opcodes.emplace_back(key, count);
    }
    return result;
}

}
