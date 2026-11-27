cmake_minimum_required(VERSION 3.25)

project(Euphoria LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "euphoria suport only x64")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _is_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

function(euphoria_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /Zc:__cplusplus
            /FS
            /utf-8
            /EHsc
            /arch:AVX2
        )

        target_compile_definitions(${target} PRIVATE
            _CRT_SECURE_NO_WARNINGS
            NOMINMAX
            WIN32_LEAN_AND_MEAN
        )

        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:/LTCG /INCREMENTAL:NO /DEBUG:FULL /OPT:REF /OPT:ICF>
        )

        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Release>:/O2 /GL /fp:fast /Zi>
            $<$<CONFIG:RelWithDebInfo>:/O2>
            $<$<CONFIG:Debug>:/Od /RTC1>
        )
    else()
        target_compile_options(${target} PRIVATE
            -m64
            -Wall
            -Wextra
            -Wpedantic
            -mavx2
        )

        target_compile_options(${target} PRIVATE
            $<$<CONFIG:Release>:-O3 -flto>
            $<$<CONFIG:RelWithDebInfo>:-O2 -g>
        )

        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:-flto>
        )
    endif()
endfunction()

add_library(euphoria_openscarlett STATIC
    third_party/openscarlett/src/core/status.cpp
    third_party/openscarlett/src/gpu/rdna2_disasm.cpp
    third_party/openscarlett/src/gpu/container_min.cpp
)

target_include_directories(euphoria_openscarlett PUBLIC third_party/openscarlett/include)

if(MSVC)
    target_compile_options(euphoria_openscarlett PRIVATE /W3 /permissive-)
endif()

add_library(euphoria_core STATIC
    src/gpu/memory.cpp
    src/gpu/swapchain.cpp
    src/gpu/command.cpp
    src/gpu/resources.cpp
    src/gpu/draw.cpp
    src/gpu/title_memory.cpp
    src/gpu/present.cpp
    src/gpu/pipeline.cpp
    src/gpu/descriptor_host.cpp
    src/formats/texture.cpp
    src/formats/bcpack.cpp
    src/formats/bcpack_tables.cpp
    src/formats/inflate.cpp
    src/formats/xma.cpp
    src/formats/xma_tables.cpp
    src/shim/xg/xg.cpp
    src/log/log.cpp
    src/log/trace.cpp
    src/loader/pe.cpp
    src/relinker/symbol_table.cpp
    src/relinker/resolver.cpp
    src/shader/codec.cpp
    src/shader/container.cpp
    src/shader/cache.cpp
    src/shader/translator.cpp
    src/shader/xbox.cpp
    src/shader/rdna2.cpp
    src/shader/lift_hlsl.cpp
    src/shader/dxc.cpp
    src/shader/aot.cpp
    src/runtime/guest.cpp
    src/vfs/vfs.cpp
    src/host/host.cpp
    src/graphics/context.cpp
    src/audio/backend.cpp
    src/input/backend.cpp
    src/shim/registry.cpp
    src/shim/xapi/xapi.cpp
    src/shim/gdk/gdk.cpp
    src/shim/kernel/kernel.cpp
    src/shim/kernel/identity.cpp
    src/shim/dx/dx.cpp
    src/shim/audio/audio.cpp
    src/shim/audio/acphal.cpp
    src/shim/input/input.cpp
    src/shim/net/net.cpp
    src/shim/live/live.cpp
    src/shim/system/system.cpp
    src/shim/system/dstorage.cpp
)

foreach(phase a1 a2 a3 b c)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/shader_${phase}.cmake OPTIONAL)
endforeach()

target_include_directories(euphoria_core PUBLIC include)

target_link_libraries(euphoria_core PUBLIC euphoria_openscarlett d3d12 dxgi user32 dbghelp)

if(WIN32)
    target_link_libraries(euphoria_core PUBLIC psapi)
endif()

euphoria_options(euphoria_core)

add_executable(Euphoria WIN32
    src/entry.cpp
)

target_link_libraries(Euphoria PRIVATE euphoria_core)

euphoria_options(Euphoria)

if(MSVC)
    target_link_options(Euphoria PRIVATE /BASE:0x180000000 /DYNAMICBASE:NO)
endif()

set_target_properties(Euphoria PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

add_executable(euphoria_shader
    src/tools/shader_tool.cpp
)

target_link_libraries(euphoria_shader PRIVATE euphoria_core)
euphoria_options(euphoria_shader)

set_target_properties(euphoria_shader PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    OUTPUT_NAME "euphoria-shader"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

foreach(target Euphoria euphoria_shader)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/dxc"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dxc/bin/x64"
            "$<TARGET_FILE_DIR:${target}>/dxc"
        VERBATIM
    )
endforeach()
