target_sources(euphoria_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/emit/bitcode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/emit/bitcode_opt.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/emit/bitcode_census.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/emit/shader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/emit/container.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/emit/validator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/emit/sampler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/tests/bitcode_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/tests/bitcode_opt_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/tests/container_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/tests/shader_test.cpp
)
