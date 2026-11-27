#include <euphoria/shader/gfx10/emit/validator.hpp>

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace euphoria::shader::gfx10::emit {
namespace {

constexpr GUID kClsidDxcValidator{0x8ca3e215, 0xf728, 0x4cf3, {0x8c, 0xdd, 0x88, 0xaf, 0x91, 0x75, 0x87, 0xa1}};
constexpr GUID kIidDxcValidator{0xa6e82bd2, 0x1fd7, 0x4826, {0x98, 0x11, 0x28, 0x57, 0xe7, 0x97, 0xf4, 0x9a}};
constexpr GUID kIidUnknown{0x00000000, 0x0000, 0x0000, {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID kIidDxcBlob{0x8ba5fb08, 0x5195, 0x40e2, {0xac, 0x58, 0x0d, 0x98, 0x9c, 0x3a, 0x01, 0x02}};
constexpr std::uint32_t kInPlaceEdit = 1;

using CreateInstance = HRESULT(WINAPI*)(const GUID*, const GUID*, void**);

template <class F>
F method(void* obj, std::size_t slot)
{
    void** table = *static_cast<void***>(obj);
    return reinterpret_cast<F>(table[slot]);
}

void release(void* obj)
{
    if (obj != nullptr) {
        method<ULONG(STDMETHODCALLTYPE*)(void*)>(obj, 2)(obj);
    }
}

struct Blob;

struct BlobVtable {
    HRESULT(STDMETHODCALLTYPE* query)(Blob*, const GUID*, void**);
    ULONG(STDMETHODCALLTYPE* add_ref)(Blob*);
    ULONG(STDMETHODCALLTYPE* release)(Blob*);
    void*(STDMETHODCALLTYPE* pointer)(Blob*);
    SIZE_T(STDMETHODCALLTYPE* size)(Blob*);
};

struct Blob {
    const BlobVtable* vtable;
    std::uint8_t* data;
    std::size_t len;
};

HRESULT STDMETHODCALLTYPE blob_query(Blob* self, const GUID* iid, void** out)
{
    if (std::memcmp(iid, &kIidUnknown, sizeof(GUID)) == 0 || std::memcmp(iid, &kIidDxcBlob, sizeof(GUID)) == 0) {
        *out = self;
        return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE blob_ref(Blob*)
{
    return 1;
}

void* STDMETHODCALLTYPE blob_pointer(Blob* self)
{
    return self->data;
}

SIZE_T STDMETHODCALLTYPE blob_size(Blob* self)
{
    return self->len;
}

constexpr BlobVtable kBlobVtable{blob_query, blob_ref, blob_ref, blob_pointer, blob_size};

std::optional<std::filesystem::path> env_path()
{
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD n = GetEnvironmentVariableW(L"EUPHORIA_DXIL", buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) {
        return std::nullopt;
    }
    return std::filesystem::path{std::wstring{buffer, n}};
}

std::optional<std::filesystem::path> next_to_exe()
{
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (n == 0 || n >= std::size(buffer)) {
        return std::nullopt;
    }
    const std::filesystem::path dir = std::filesystem::path{std::wstring{buffer, n}}.parent_path();
    for (const auto& candidate : {dir / L"dxc" / L"dxil.dll", dir / L"dxil.dll"}) {
        std::error_code ec;
        if (std::filesystem::exists(candidate, ec)) {
            return candidate;
        }
    }
    return std::nullopt;
}

}

Validator::Validator(Validator&& other) noexcept
    : object_(std::exchange(other.object_, nullptr))
{
}

Validator& Validator::operator=(Validator&& other) noexcept
{
    if (this != &other) {
        release(object_);
        object_ = std::exchange(other.object_, nullptr);
    }
    return *this;
}

Validator::~Validator()
{
    release(object_);
}

std::optional<std::filesystem::path> Validator::default_path()
{
    if (auto p = env_path()) {
        return p;
    }
    const std::filesystem::path kits{L"C:\\Program Files (x86)\\Windows Kits\\10\\bin"};
    std::vector<std::filesystem::path> versions;
    std::error_code ec;
    for (std::filesystem::directory_iterator it{kits, ec}, end; !ec && it != end; it.increment(ec)) {
        const std::filesystem::path dll = it->path() / L"x64" / L"dxil.dll";
        std::error_code exists_ec;
        if (std::filesystem::exists(dll, exists_ec)) {
            versions.push_back(dll);
        }
    }
    if (!versions.empty()) {
        std::sort(versions.begin(), versions.end());
        return versions.back();
    }
    return next_to_exe();
}

std::expected<Validator, std::string> Validator::load(const std::optional<std::filesystem::path>& path)
{
    const std::wstring name = path ? path->wstring() : std::wstring{L"dxil.dll"};
    HMODULE module = LoadLibraryW(name.c_str());
    if (module == nullptr) {
        return std::unexpected(std::format("cannot load {}", std::filesystem::path{name}.string()));
    }
    const auto create = reinterpret_cast<CreateInstance>(reinterpret_cast<void*>(GetProcAddress(module, "DxcCreateInstance")));
    if (create == nullptr) {
        return std::unexpected(std::string{"dxil.dll lacks DxcCreateInstance"});
    }
    void* object = nullptr;
    const HRESULT hr = create(&kClsidDxcValidator, &kIidDxcValidator, &object);
    if (FAILED(hr) || object == nullptr) {
        return std::unexpected(std::format("DxcCreateInstance(validator) failed: {:#x}", static_cast<std::uint32_t>(hr)));
    }
    Validator v;
    v.object_ = object;
    return v;
}

std::expected<void, std::string> Validator::validate(std::span<std::uint8_t> container)
{
    if (object_ == nullptr) {
        return std::unexpected(std::string{"no validator"});
    }
    Blob blob{&kBlobVtable, container.data(), container.size()};
    void* result = nullptr;
    const auto validate_fn = method<HRESULT(STDMETHODCALLTYPE*)(void*, Blob*, std::uint32_t, void**)>(object_, 3);
    const HRESULT hr = validate_fn(object_, &blob, kInPlaceEdit, &result);
    if (FAILED(hr) || result == nullptr) {
        return std::unexpected(std::format("Validate failed: {:#x}", static_cast<std::uint32_t>(hr)));
    }
    HRESULT status = S_OK;
    method<HRESULT(STDMETHODCALLTYPE*)(void*, HRESULT*)>(result, 3)(result, &status);
    std::string text;
    if (FAILED(status)) {
        void* errors = nullptr;
        const auto get_errors = method<HRESULT(STDMETHODCALLTYPE*)(void*, void**)>(result, 5);
        if (SUCCEEDED(get_errors(result, &errors)) && errors != nullptr) {
            const auto ptr = method<const char*(STDMETHODCALLTYPE*)(void*)>(errors, 3)(errors);
            const auto len = method<SIZE_T(STDMETHODCALLTYPE*)(void*)>(errors, 4)(errors);
            std::string_view bytes{ptr, len};
            while (!bytes.empty() && (bytes.back() == '\0' || bytes.back() == ' ' || bytes.back() == '\n' || bytes.back() == '\r' || bytes.back() == '\t')) {
                bytes.remove_suffix(1);
            }
            while (!bytes.empty() && (bytes.front() == ' ' || bytes.front() == '\n' || bytes.front() == '\r' || bytes.front() == '\t')) {
                bytes.remove_prefix(1);
            }
            text = std::string{bytes};
            release(errors);
        }
        if (text.empty()) {
            text = std::format("rejected ({:#x})", static_cast<std::uint32_t>(status));
        }
    }
    release(result);
    if (FAILED(status)) {
        return std::unexpected(std::move(text));
    }
    return {};
}

}
