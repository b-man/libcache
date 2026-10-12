cmake_minimum_required(VERSION 3.15)
project(NativeCacheSystem C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(DEBUG "Build with debug symbols and AddressSanitizer" OFF)
option(PRIVATE "Export private API symbols and include private API unit tests" ON)

set(VERSION "1000" CACHE STRING "Library version")

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -mmacosx-version-min=11.0 -fapplication-extension")
if(DEBUG)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g -fsanitize=address")
else()
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3")
endif()

if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
    set(CMAKE_INSTALL_PREFIX "/usr" CACHE PATH "Installation prefix" FORCE)
endif()
include(GNUInstallDirs)

add_library(cache SHARED 
    src/cache_callbacks.c 
    src/cache_common.c 
    src/cache_diagnostics.c 
    src/cache_eviction.c
    src/cache_queue.c
)

target_include_directories(cache PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
if(PRIVATE)
    target_compile_definitions(cache PRIVATE CACHE_PRIVATE)
endif()

set_target_properties(cache PROPERTIES
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    NO_SONAME ON
)

target_link_options(cache PRIVATE
    -install_name "${CMAKE_INSTALL_FULL_LIBDIR}/system/libcache.dylib"
    -current_version ${VERSION}
    -compatibility_version 1.0.0
    -umbrella System
)

target_link_libraries(cache PRIVATE pthread)

add_custom_command(TARGET cache POST_BUILD
    COMMAND xcrun tapi stubify --filetype=tbd-v4 "$<TARGET_FILE:cache>" -o "${CMAKE_CURRENT_BINARY_DIR}/libcache.tbd"
    BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/libcache.tbd"
    COMMENT "Generating libcache.tbd text-based stub from libcache.dylib"
    VERBATIM
)

set(UNIT_TEST_SOURCES
    tests/unit_tests.c
    tests/test_callbacks.c
    tests/test_core.c
)
if(PRIVATE)
    list(APPEND UNIT_TEST_SOURCES tests/test_private.c)
endif()

add_executable(cache_unit_tests ${UNIT_TEST_SOURCES})

if(PRIVATE)
    target_compile_definitions(cache_unit_tests PRIVATE CACHE_PRIVATE)
endif()
target_link_libraries(cache_unit_tests PRIVATE cache pthread)

add_executable(cache_stress
    perf/cache_stress.c
)

target_link_libraries(cache_stress PRIVATE cache pthread)

set(INSTALL_HEADERS
    include/cache.h
    include/cache_callbacks.h
)
if(PRIVATE)
    list(APPEND INSTALL_HEADERS
        include/cache_private.h
    )
endif()

install(TARGETS cache
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/system
)

install(FILES "${CMAKE_CURRENT_BINARY_DIR}/libcache.tbd"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/system
)

install(FILES ${INSTALL_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

