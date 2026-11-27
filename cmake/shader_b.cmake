target_sources(euphoria_core PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/structure.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/spill.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/bvh.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/io.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/resource.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/resource_lanes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/resource_fixtures.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/tests/structure_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/tests/spill_test.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shader/gfx10/tests/bvh_test.cpp
)
