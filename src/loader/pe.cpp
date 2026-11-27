#include <euphoria/loader/pe.hpp>

#include <euphoria/log/log.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#include <utility>

#include <windows.h>

namespace euphoria::loader {
namespace {

constexpr std::size_t kDosHeaderSize = 0x40;
constexpr std::size_t kCoffHeaderSize = 20;
constexpr std::size_t kSectionHeaderSize = 40;
constexpr std::size_t kImportDescriptorSize = 20;
constexpr std::size_t kDataDirectoryOffset = 112;
constexpr std::uint16_t kMachineAmd64 = 0x8664;
constexpr std::uint16_t kMagicPe32Plus = 0x20B;
constexpr std::uint16_t kRelocDir64 = 10;
constexpr std::uint16_t kFileDll = 0x2000;
constexpr std::uint16_t kDllForceIntegrity = 0x80;
constexpr std::uint16_t kDllDynamicBase = 0x40;
constexpr std::uint16_t kDllHighEntropyVa = 0x20;
constexpr std::uint32_t kSizeOfHeadersOffset = 60;
constexpr std::uint32_t kChecksumOffset = 64;
constexpr std::uint32_t kDllCharacteristicsOffset = 70;
constexpr std::uint32_t kStackReserveOffset = 72;
constexpr std::uint32_t kImageBaseOffset = 24;
constexpr std::uint32_t kEntryPointOffset = 16;
constexpr std::uint32_t kSubsystemOffset = 68;
constexpr std::uint32_t kNumRvaAndSizesOffset = 108;

constexpr std::size_t kExportDirectory = 0;
constexpr std::size_t kImportDirectory = 1;
constexpr std::size_t kBaseRelocDirectory = 5;
constexpr std::size_t kTlsDirectory = 9;
constexpr std::size_t kBoundImportDirectory = 11;
constexpr std::size_t kDelayImportDirectory = 13;

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

template <typename T>
T read_at(std::span<const std::uint8_t> data, std::size_t offset) noexcept
{
    T value{};
    if (offset + sizeof(T) <= data.size()) {
        std::memcpy(&value, data.data() + offset, sizeof(T));
    }
    return value;
}

std::string read_string(std::span<const std::uint8_t> data, std::size_t offset, std::size_t limit)
{
    if (offset >= data.size()) {
        return {};
    }
    std::size_t end = offset;
    while (end < data.size() && end < offset + limit && data[end] != 0) {
        ++end;
    }
    return std::string(reinterpret_cast<const char*>(data.data() + offset), end - offset);
}

bool write_at(std::vector<std::uint8_t>& data, std::size_t offset, const void* value, std::size_t size)
{
    if (offset + size > data.size()) {
        return false;
    }
    std::memcpy(data.data() + offset, value, size);
    return true;
}

bool write_u16(std::vector<std::uint8_t>& data, std::size_t offset, std::uint16_t value)
{
    return write_at(data, offset, &value, sizeof(value));
}

bool write_u32(std::vector<std::uint8_t>& data, std::size_t offset, std::uint32_t value)
{
    return write_at(data, offset, &value, sizeof(value));
}

bool write_u64(std::vector<std::uint8_t>& data, std::size_t offset, std::uint64_t value)
{
    return write_at(data, offset, &value, sizeof(value));
}

std::string hex(std::uint64_t value)
{
    char text[32]{};
    std::snprintf(text, sizeof(text), "%llx", static_cast<unsigned long long>(value));
    return text;
}

std::uint64_t fnv1a(std::span<const std::uint8_t> data) noexcept
{
    std::uint64_t hash = kFnvOffset;
    for (const std::uint8_t byte : data) {
        hash ^= byte;
        hash *= kFnvPrime;
    }
    return hash;
}

}

Image::Image(Image&& other) noexcept
    : bytes_(std::move(other.bytes_))
    , sections_(std::move(other.sections_))
    , imports_(std::move(other.imports_))
    , exports_(std::move(other.exports_))
    , relocations_(std::move(other.relocations_))
    , tls_(other.tls_)
    , base_(other.base_)
    , preferred_base_(other.preferred_base_)
    , entry_rva_(other.entry_rva_)
    , size_of_image_(other.size_of_image_)
    , headers_size_(other.headers_size_)
    , stack_reserve_(other.stack_reserve_)
    , import_terminator_rva_(other.import_terminator_rva_)
    , tls_directory_rva_(other.tls_directory_rva_)
    , tls_index_rva_(other.tls_index_rva_)
    , module_path_(std::move(other.module_path_))
{
    other.base_ = nullptr;
}

Image& Image::operator=(Image&& other) noexcept
{
    if (this != &other) {
        unload_module();
        bytes_ = std::move(other.bytes_);
        sections_ = std::move(other.sections_);
        imports_ = std::move(other.imports_);
        exports_ = std::move(other.exports_);
        relocations_ = std::move(other.relocations_);
        tls_ = other.tls_;
        base_ = other.base_;
        preferred_base_ = other.preferred_base_;
        entry_rva_ = other.entry_rva_;
        size_of_image_ = other.size_of_image_;
        headers_size_ = other.headers_size_;
        stack_reserve_ = other.stack_reserve_;
        import_terminator_rva_ = other.import_terminator_rva_;
        tls_directory_rva_ = other.tls_directory_rva_;
        tls_index_rva_ = other.tls_index_rva_;
        module_path_ = std::move(other.module_path_);
        other.base_ = nullptr;
    }
    return *this;
}

Image::~Image()
{
    unload_module();
}

bool Image::is_pe64(std::span<const std::uint8_t> data) noexcept
{
    if (data.size() < kDosHeaderSize) {
        return false;
    }
    if (data[0] != 'M' || data[1] != 'Z') {
        return false;
    }

    const std::uint32_t lfanew = read_at<std::uint32_t>(data, 0x3C);
    const std::size_t pe_offset = lfanew;
    if (pe_offset + 4 + kCoffHeaderSize > data.size()) {
        return false;
    }
    if (std::memcmp(data.data() + pe_offset, "PE\0\0", 4) != 0) {
        return false;
    }
    if (read_at<std::uint16_t>(data, pe_offset + 4) != kMachineAmd64) {
        return false;
    }

    const std::size_t optional_offset = pe_offset + 4 + kCoffHeaderSize;
    return read_at<std::uint16_t>(data, optional_offset) == kMagicPe32Plus;
}

std::optional<Image> Image::load_file(const std::filesystem::path& path)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        log::error("loader: image not found: " + path.string());
        return std::nullopt;
    }

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) {
        log::error("loader: cannot open image: " + path.string());
        return std::nullopt;
    }

    const std::streamoff file_size = input.tellg();
    if (file_size <= 0) {
        log::error("loader: empty image: " + path.string());
        return std::nullopt;
    }

    Image image;
    image.bytes_.resize(static_cast<std::size_t>(file_size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(image.bytes_.data()), file_size);
    if (!input) {
        log::error("loader: short read: " + path.string());
        return std::nullopt;
    }

    if (!is_pe64(image.bytes_)) {
        log::error("loader: not an x86_64 PE image: " + path.string());
        return std::nullopt;
    }

    const std::span<const std::uint8_t> data{image.bytes_};
    const std::uint32_t lfanew = read_at<std::uint32_t>(data, 0x3C);
    const std::size_t optional_offset = lfanew + 4 + kCoffHeaderSize;
    const std::uint16_t section_count = read_at<std::uint16_t>(data, lfanew + 4 + 2);
    const std::uint16_t optional_size = read_at<std::uint16_t>(data, lfanew + 4 + 16);
    const std::uint16_t subsystem = read_at<std::uint16_t>(data, optional_offset + kSubsystemOffset);
    const std::uint32_t num_rva = read_at<std::uint32_t>(data, optional_offset + kNumRvaAndSizesOffset);

    image.preferred_base_ = read_at<std::uint64_t>(data, optional_offset + kImageBaseOffset);
    image.entry_rva_ = read_at<std::uint32_t>(data, optional_offset + kEntryPointOffset);
    image.size_of_image_ = read_at<std::uint32_t>(data, optional_offset + 56);
    image.headers_size_ = read_at<std::uint32_t>(data, optional_offset + kSizeOfHeadersOffset);
    image.stack_reserve_ = static_cast<std::uint32_t>(
        read_at<std::uint64_t>(data, optional_offset + kStackReserveOffset));

    const std::size_t section_offset = optional_offset + optional_size;
    for (std::uint16_t index = 0; index < section_count; ++index) {
        const std::size_t entry = section_offset + index * kSectionHeaderSize;
        if (entry + kSectionHeaderSize > data.size()) {
            log::error("loader: truncated section table");
            return std::nullopt;
        }

        Section section;
        section.name = read_string(data.subspan(entry, 8), 0, 8);
        section.virtual_size = read_at<std::uint32_t>(data, entry + 8);
        section.virtual_address = read_at<std::uint32_t>(data, entry + 12);
        section.raw_size = read_at<std::uint32_t>(data, entry + 16);
        section.raw_offset = read_at<std::uint32_t>(data, entry + 20);
        section.characteristics = read_at<std::uint32_t>(data, entry + 36);
        image.sections_.push_back(std::move(section));
    }

    const auto rva_to_offset = [&image](std::uint32_t rva) -> std::size_t {
        for (const auto& section : image.sections_) {
            const std::uint32_t span = std::max(section.virtual_size, section.raw_size);
            if (rva >= section.virtual_address && rva < section.virtual_address + span) {
                return static_cast<std::size_t>(section.raw_offset) + (rva - section.virtual_address);
            }
        }
        return static_cast<std::size_t>(rva);
    };

    const std::size_t directory_offset = optional_offset + kDataDirectoryOffset;
    const auto directory = [&](std::size_t index) {
        if (index >= num_rva) {
            return std::pair<std::uint32_t, std::uint32_t>{0, 0};
        }
        return std::pair<std::uint32_t, std::uint32_t>{
            read_at<std::uint32_t>(data, directory_offset + index * 8),
            read_at<std::uint32_t>(data, directory_offset + index * 8 + 4),
        };
    };

    const auto [export_rva, export_size] = directory(kExportDirectory);
    if (export_rva != 0 && export_size >= 40) {
        const std::size_t offset = rva_to_offset(export_rva);
        const std::uint32_t name_count = read_at<std::uint32_t>(data, offset + 24);
        const std::uint32_t function_base = read_at<std::uint32_t>(data, offset + 28);
        const std::uint32_t name_base = read_at<std::uint32_t>(data, offset + 32);
        const std::uint32_t ordinal_base = read_at<std::uint32_t>(data, offset + 36);
        const std::uint32_t ordinal_base_value = read_at<std::uint32_t>(data, offset + 16);

        for (std::uint32_t index = 0; index < name_count; ++index) {
            const std::uint32_t name_rva = read_at<std::uint32_t>(data, rva_to_offset(name_base) + index * 4);
            const std::uint16_t ordinal_index = read_at<std::uint16_t>(data, rva_to_offset(ordinal_base) + index * 2);
            const std::uint32_t function_rva = read_at<std::uint32_t>(data, rva_to_offset(function_base) + ordinal_index * 4);

            Export symbol;
            symbol.name = read_string(data, rva_to_offset(name_rva), 256);
            symbol.ordinal = ordinal_base_value + ordinal_index;
            symbol.rva = function_rva;
            image.exports_.push_back(std::move(symbol));
        }
    }

    const auto [import_rva, import_size] = directory(kImportDirectory);
    if (import_rva != 0 && import_size != 0) {
        std::size_t descriptor = rva_to_offset(import_rva);
        std::uint32_t descriptor_count = 0;
        while (descriptor + kImportDescriptorSize <= image.bytes_.size()) {
            const std::uint32_t original_first_thunk = read_at<std::uint32_t>(data, descriptor);
            const std::uint32_t name_rva = read_at<std::uint32_t>(data, descriptor + 12);
            const std::uint32_t first_thunk = read_at<std::uint32_t>(data, descriptor + 16);
            if (name_rva == 0) {
                break;
            }

            const std::string module = read_string(data, rva_to_offset(name_rva), 256);
            const std::uint32_t thunk_rva = original_first_thunk != 0 ? original_first_thunk : first_thunk;
            std::size_t thunk_offset = rva_to_offset(thunk_rva);
            std::uint32_t index = 0;
            while (thunk_offset + 8 <= image.bytes_.size()) {
                const std::uint64_t entry = read_at<std::uint64_t>(data, thunk_offset);
                if (entry == 0) {
                    break;
                }

                Import import;
                import.module = module;
                import.iat_rva = first_thunk + index * 8;
                if ((entry & (1ULL << 63)) != 0) {
                    import.by_ordinal = true;
                    import.ordinal = static_cast<std::uint32_t>(entry & 0xFFFF);
                } else {
                    const std::uint32_t hint_name_rva = static_cast<std::uint32_t>(entry & 0x7FFFFFFF);
                    import.name = read_string(data, rva_to_offset(hint_name_rva) + 2, 256);
                }
                image.imports_.push_back(std::move(import));
                thunk_offset += 8;
                ++index;
            }
            descriptor += kImportDescriptorSize;
            ++descriptor_count;
        }
        image.import_terminator_rva_ = import_rva + descriptor_count * kImportDescriptorSize;
    }

    const auto [reloc_rva, reloc_size] = directory(kBaseRelocDirectory);
    if (reloc_rva != 0 && reloc_size >= 8) {
        const std::size_t start = rva_to_offset(reloc_rva);
        std::size_t offset = start;
        const std::size_t end = std::min(start + reloc_size, image.bytes_.size());
        while (offset + 8 <= end) {
            const std::uint32_t page_rva = read_at<std::uint32_t>(data, offset);
            const std::uint32_t block_size = read_at<std::uint32_t>(data, offset + 4);
            if (block_size < 8 || offset + block_size > end) {
                break;
            }
            const std::size_t entry_count = (block_size - 8) / 2;
            for (std::size_t i = 0; i < entry_count; ++i) {
                const std::size_t entry_offset = offset + 8 + i * 2;
                const std::uint16_t entry = read_at<std::uint16_t>(data, entry_offset);
                const std::uint16_t type = static_cast<std::uint16_t>(entry >> 12);
                if (type != 0) {
                    image.relocations_.push_back(
                        {page_rva + (entry & 0xFFF), static_cast<std::uint32_t>(entry_offset), type});
                }
            }
            offset += block_size;
        }
    }

    const auto [tls_rva, tls_size] = directory(kTlsDirectory);
    if (tls_rva != 0 && tls_size >= 40) {
        const std::size_t offset = rva_to_offset(tls_rva);
        image.tls_directory_rva_ = tls_rva;
        image.tls_.start_va = read_at<std::uint64_t>(data, offset);
        image.tls_.end_va = read_at<std::uint64_t>(data, offset + 8);
        image.tls_.index_va = read_at<std::uint64_t>(data, offset + 16);
        image.tls_.callbacks_va = read_at<std::uint64_t>(data, offset + 24);
        if (image.tls_.index_va >= image.preferred_base_) {
            image.tls_index_rva_ = static_cast<std::uint32_t>(image.tls_.index_va - image.preferred_base_);
        }
    }

    log::info("loader: image ok, subsystem " + std::to_string(subsystem)
              + ", size " + std::to_string(image.size_of_image_)
              + ", imports " + std::to_string(image.imports_.size())
              + ", exports " + std::to_string(image.exports_.size())
              + ", relocations " + std::to_string(image.relocations_.size()));
    return image;
}

bool Image::load_as_module(const std::filesystem::path& cache_directory)
{
    if (base_ != nullptr) {
        return true;
    }
    if (bytes_.empty()) {
        log::error("loader: image bytes are gone before loading");
        return false;
    }

    const std::uint32_t lfanew = read_at<std::uint32_t>(bytes_, 0x3C);
    const std::size_t file_header = lfanew + 4;
    const std::size_t optional_offset = file_header + kCoffHeaderSize;
    const std::size_t directory_offset = optional_offset + kDataDirectoryOffset;
    const auto directory_entry = [&](std::size_t index) {
        return directory_offset + index * 8;
    };

    write_u16(bytes_, file_header + 18, static_cast<std::uint16_t>(
        read_at<std::uint16_t>(bytes_, file_header + 18) | kFileDll));
    write_u32(bytes_, optional_offset + kEntryPointOffset, 0);
    write_u32(bytes_, optional_offset + kChecksumOffset, 0);
    write_u16(bytes_, optional_offset + kDllCharacteristicsOffset, static_cast<std::uint16_t>(
        read_at<std::uint16_t>(bytes_, optional_offset + kDllCharacteristicsOffset)
        & ~(kDllForceIntegrity | kDllDynamicBase | kDllHighEntropyVa)));

    if (import_terminator_rva_ != 0) {
        write_u32(bytes_, directory_entry(kImportDirectory), import_terminator_rva_);
        write_u32(bytes_, directory_entry(kImportDirectory) + 4, kImportDescriptorSize);
    }
    write_u64(bytes_, directory_entry(kBoundImportDirectory), 0);
    write_u64(bytes_, directory_entry(kDelayImportDirectory), 0);

    if (tls_directory_rva_ != 0) {
        const std::size_t tls_offset = [&] {
            for (const auto& section : sections_) {
                const std::uint32_t span = std::max(section.virtual_size, section.raw_size);
                if (tls_directory_rva_ >= section.virtual_address
                    && tls_directory_rva_ < section.virtual_address + span) {
                    return static_cast<std::size_t>(section.raw_offset)
                         + (tls_directory_rva_ - section.virtual_address);
                }
            }
            return static_cast<std::size_t>(tls_directory_rva_);
        }();

        write_u64(bytes_, tls_offset + 24, 0);
        const std::uint32_t callbacks_rva = tls_directory_rva_ + 24;
        for (const auto& relocation : relocations_) {
            if (relocation.rva == callbacks_rva) {
                write_u16(bytes_, relocation.file_offset, 0);
            }
        }
    }

    const std::uint64_t hash = fnv1a(bytes_);
    std::error_code ec;
    std::filesystem::create_directories(cache_directory, ec);

    const std::filesystem::path output =
        cache_directory / ("euphoria-image-v2-" + hex(hash) + ".dll");
    std::uintmax_t existing = 0;
    if (std::filesystem::is_regular_file(output, ec)) {
        existing = std::filesystem::file_size(output, ec);
    }
    if (ec || existing != bytes_.size()) {
        std::ofstream file(output, std::ios::binary | std::ios::trunc);
        if (!file) {
            log::error("loader: cannot write prepared image " + output.string());
            return false;
        }
        file.write(reinterpret_cast<const char*>(bytes_.data()),
                   static_cast<std::streamsize>(bytes_.size()));
        file.close();
        if (!file) {
            log::error("loader: cannot write prepared image " + output.string());
            return false;
        }
    }

    log::info("loader: loading prepared image " + output.string());
    bytes_.clear();
    bytes_.shrink_to_fit();

    HMODULE module = LoadLibraryExW(output.wstring().c_str(), nullptr, 0);
    if (module == nullptr) {
        log::error("loader: LoadLibraryEx failed, error " + std::to_string(GetLastError()));
        return false;
    }

    base_ = reinterpret_cast<std::uint8_t*>(module);
    module_path_ = output;
    log::info("loader: mapped at 0x" + hex(reinterpret_cast<std::uintptr_t>(base_))
              + " (preferred 0x" + hex(preferred_base_) + ")");
    return true;
}

void Image::unload_module() noexcept
{
    if (base_ != nullptr) {
        FreeLibrary(reinterpret_cast<HMODULE>(base_));
        base_ = nullptr;
    }
}

std::uint8_t* Image::at(std::uint32_t rva) const noexcept
{
    return base_ != nullptr ? base_ + rva : nullptr;
}

std::uintptr_t Image::entry_point() const noexcept
{
    return base_ != nullptr ? reinterpret_cast<std::uintptr_t>(base_ + entry_rva_) : 0;
}

std::uintptr_t Image::tls_callbacks() const noexcept
{
    if (base_ == nullptr || tls_directory_rva_ == 0) {
        return 0;
    }
    const auto* directory = reinterpret_cast<const std::uint64_t*>(base_ + tls_directory_rva_);
    return static_cast<std::uintptr_t>(directory[3]);
}

std::uintptr_t Image::tls_callbacks_original() const noexcept
{
    if (base_ == nullptr || tls_.callbacks_va == 0 || tls_.callbacks_va < preferred_base_) {
        return 0;
    }
    return reinterpret_cast<std::uintptr_t>(base_)
         + static_cast<std::uintptr_t>(tls_.callbacks_va - preferred_base_);
}

std::uint32_t Image::tls_index() const noexcept
{
    if (base_ == nullptr || tls_index_rva_ == 0) {
        return 0;
    }
    return *reinterpret_cast<const std::uint32_t*>(base_ + tls_index_rva_);
}

bool Image::redirect_tls()
{
    if (base_ == nullptr || tls_index_rva_ == 0) {
        return true;
    }

    const std::uint32_t index = tls_index();
    if (index == 0) {
        return true;
    }

    constexpr std::size_t thunk_size = 32;
    constexpr std::size_t thunk_page_size = 4096;
    constexpr std::size_t granule = 0x10000;
    constexpr std::size_t load_length = 9;

    std::uint8_t* page = nullptr;
    const std::uintptr_t image_begin = reinterpret_cast<std::uintptr_t>(base_);
    const std::uintptr_t image_end = image_begin + size_of_image_;
    const std::uintptr_t above = (image_end + granule - 1) & ~(granule - 1);
    for (std::size_t step = 0; step < (1U << 14) && page == nullptr; ++step) {
        const std::uintptr_t below = image_begin > (step + 1) * granule
                                      ? image_begin - (step + 1) * granule
                                      : 0;
        const std::uintptr_t hints[2]{below, above + step * granule};
        for (const std::uintptr_t hint : hints) {
            if (hint == 0) {
                continue;
            }
            page = static_cast<std::uint8_t*>(
                VirtualAlloc(reinterpret_cast<void*>(hint), thunk_page_size, MEM_COMMIT | MEM_RESERVE,
                             PAGE_READWRITE));
            if (page != nullptr) {
                break;
            }
        }
    }
    if (page == nullptr) {
        log::error("loader: no free page near the image for TLS thunks");
        return false;
    }

    const std::uint32_t displacement = index * 8;
    for (std::uint8_t reg = 0; reg < 16; ++reg) {
        const std::uint8_t high = reg >> 3;
        const std::uint8_t low = reg & 7;
        std::uint8_t thunk[24]{};
        std::size_t length = 0;

        thunk[length++] = 0x65;
        thunk[length++] = static_cast<std::uint8_t>(0x48 | (high << 2));
        thunk[length++] = 0x8B;
        thunk[length++] = static_cast<std::uint8_t>((low << 3) | 4);
        thunk[length++] = 0x25;
        thunk[length++] = 0x58;
        thunk[length++] = 0;
        thunk[length++] = 0;
        thunk[length++] = 0;

        thunk[length++] = static_cast<std::uint8_t>(0x48 | (high << 2) | high);
        thunk[length++] = 0x8D;
        thunk[length++] = static_cast<std::uint8_t>(0x80 | (low << 3) | low);
        if (low == 4) {
            thunk[length++] = 0x24;
        }
        std::memcpy(thunk + length, &displacement, sizeof(displacement));
        length += sizeof(displacement);
        thunk[length++] = 0xC3;

        std::memcpy(page + reg * thunk_size, thunk, length);
    }

    DWORD previous = 0;
    VirtualProtect(page, thunk_page_size, PAGE_EXECUTE_READ, &previous);
    FlushInstructionCache(GetCurrentProcess(), page, thunk_page_size);

    std::size_t patched = 0;
    for (const auto& section : sections_) {
        if ((section.characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }
        auto* start = base_ + section.virtual_address;
        const std::size_t length = section.virtual_size;
        if (VirtualProtect(start, length, PAGE_EXECUTE_READWRITE, &previous) == 0) {
            log::error("loader: cannot unprotect code section " + section.name);
            return false;
        }

        for (std::size_t offset = 0; offset + load_length <= length; ++offset) {
            const std::uint8_t* c = start + offset;
            const std::uint8_t rex = c[1];
            const std::uint8_t reg = static_cast<std::uint8_t>(((c[3] >> 3) & 7) | ((rex & 4) != 0 ? 8 : 0));
            if (c[0] != 0x65 || (rex != 0x48 && rex != 0x4C) || c[2] != 0x8B
                || (c[3] & 0xC7) != 0x04 || c[4] != 0x25 || c[5] != 0x58 || c[6] != 0
                || c[7] != 0 || c[8] != 0 || reg == 4) {
                continue;
            }

            const auto site = reinterpret_cast<std::uintptr_t>(start + offset);
            const auto thunk = reinterpret_cast<std::uintptr_t>(page + reg * thunk_size);
            const auto relative = static_cast<std::int32_t>(thunk - (site + 5));
            start[offset] = 0xE8;
            std::memcpy(start + offset + 1, &relative, sizeof(relative));
            start[offset + 5] = 0x0F;
            start[offset + 6] = 0x1F;
            start[offset + 7] = 0x40;
            start[offset + 8] = 0x00;
            ++patched;
            offset += load_length - 1;
        }

        VirtualProtect(start, length, previous, &previous);
        FlushInstructionCache(GetCurrentProcess(), start, length);
    }

    *reinterpret_cast<std::uint32_t*>(base_ + tls_index_rva_) = 0;
    log::info("loader: TLS index " + std::to_string(index) + " redirected at "
              + std::to_string(patched) + " sites");
    return true;
}

std::uintptr_t Image::find_export(std::string_view name) const noexcept
{
    if (base_ == nullptr) {
        return 0;
    }
    for (const auto& symbol : exports_) {
        if (symbol.name == name && symbol.rva != 0) {
            return reinterpret_cast<std::uintptr_t>(base_ + symbol.rva);
        }
    }
    return 0;
}

}
