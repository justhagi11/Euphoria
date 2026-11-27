#pragma once

#include <cstdint>
#include <string>

namespace euphoria::shim::kernel {

void set_main_image(std::uintptr_t base, const wchar_t* path);
bool is_main_image(std::uintptr_t base) noexcept;
std::uintptr_t main_image() noexcept;
void set_tls_callbacks(std::uintptr_t callbacks) noexcept;
std::uintptr_t tls_callbacks() noexcept;

void* __stdcall get_module_handle_w(const wchar_t* name);
void* __stdcall get_module_handle_a(const char* name);
void* __stdcall get_proc_address(void* module, const char* name);
std::uint32_t __stdcall get_module_file_name_w(void* module, wchar_t* buffer, std::uint32_t size);
std::uint32_t __stdcall get_module_file_name_a(void* module, char* buffer, std::uint32_t size);

void* __stdcall create_file_w(const wchar_t* path, std::uint32_t access, std::uint32_t share,
                              void* attributes, std::uint32_t disposition, std::uint32_t flags,
                              void* template_file);
void* __stdcall create_file_a(const char* path, std::uint32_t access, std::uint32_t share,
                              void* attributes, std::uint32_t disposition, std::uint32_t flags,
                              void* template_file);
std::uint32_t __stdcall get_file_attributes_w(const wchar_t* path);
std::uint32_t __stdcall get_file_attributes_a(const char* path);
int __stdcall create_directory_w(const wchar_t* path, void* attributes);
int __stdcall create_directory_a(const char* path, void* attributes);
int __stdcall delete_file_w(const wchar_t* path);
int __stdcall delete_file_a(const char* path);
int __stdcall move_file_ex_w(const wchar_t* from, const wchar_t* to, std::uint32_t flags);
int __stdcall move_file_ex_a(const char* from, const char* to, std::uint32_t flags);
void* __stdcall find_first_file_w(const wchar_t* pattern, void* data);
void* __stdcall find_first_file_a(const char* pattern, void* data);
bool remap_guest_path(const wchar_t* path, std::wstring& output);
std::int16_t __stdcall register_class_ex_a(const void* description);
std::int16_t __stdcall register_class_ex_w(const void* description);
void* __stdcall create_window_ex_a(std::uint32_t ex_style, const char* class_name,
                                   const char* window_name, std::uint32_t style, int x, int y,
                                   int width, int height, void* parent, void* menu,
                                   void* instance, void* parameter);
void* __stdcall create_window_ex_w(std::uint32_t ex_style, const wchar_t* class_name,
                                   const wchar_t* window_name, std::uint32_t style, int x, int y,
                                   int width, int height, void* parent, void* menu,
                                   void* instance, void* parameter);

}
