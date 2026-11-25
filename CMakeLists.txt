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

add_executable(Euphoria
    src/entry.cpp
)

set_target_properties(Euphoria PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

if(MSVC)
    target_compile_options(Euphoria PRIVATE
        /W4
        /permissive-
        /Zc:__cplusplus
        /utf-8
        /EHsc
        /arch:AVX2
    )

    target_compile_definitions(Euphoria PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )

    target_link_options(Euphoria PRIVATE
        $<$<CONFIG:Release>:/LTCG /INCREMENTAL:NO>
    )

    target_compile_options(Euphoria PRIVATE
        $<$<CONFIG:Release>:/O2 /GL /fp:fast>
        $<$<CONFIG:RelWithDebInfo>:/O2>
        $<$<CONFIG:Debug>:/Od /RTC1>
    )
else()
    target_compile_options(Euphoria PRIVATE
        -m64
        -Wall
        -Wextra
        -Wpedantic
        -mavx2
    )

    target_compile_options(Euphoria PRIVATE
        $<$<CONFIG:Release>:-O3 -flto>
        $<$<CONFIG:RelWithDebInfo>:-O2 -g>
    )

    target_link_options(Euphoria PRIVATE
        $<$<CONFIG:Release>:-flto>
    )
endif()
