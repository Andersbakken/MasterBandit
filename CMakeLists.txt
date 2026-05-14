cmake_minimum_required(VERSION 3.16)

# macOS deployment floor. Must be set before project() so the toolchain
# initialization picks it up. 12.0 (Monterey) matches the prebuilt Dawn
# tarball's own floor, so the binary's effective minimum is consistent
# across our code, Dawn, and any future vcpkg rebuilds at the same target.
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0" CACHE STRING "macOS deployment target")
endif()

project(masterbandit LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Build options ─────────────────────────────────────────────────────────────
option(ASAN          "Enable AddressSanitizer"                        OFF)
option(TSAN          "Enable ThreadSanitizer"                         OFF)
option(MB_ADHOC_SIGN "Ad-hoc codesign the macOS .app after each build" ON)

if(ASAN AND TSAN)
    message(FATAL_ERROR "ASAN and TSAN cannot be enabled simultaneously")
endif()
if(ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()

# Release builds carry -g so backtrace_symbols_fd in the crash handler
# can produce useful frames, and so a stripped offline copy of the
# binary can still be decoded with addr2line. -g is independent of -O
# level so runtime perf is unaffected; cost is binary size + link time.
# RelWithDebInfo (the "profile" preset) already includes -g by default.
add_compile_options($<$<CONFIG:Release>:-g>)

# On ELF platforms backtrace_symbols_fd resolves names via the dynamic
# symbol table. Without -rdynamic only exported symbols are visible,
# which strips essentially every internal frame from the crash trace.
# macOS uses dladdr() internally and doesn't need this.
if(NOT APPLE)
    add_link_options(-rdynamic)
endif()

# Where bundled assets (shaders, fonts, scripts) get copied. On macOS this is
# inside the .app bundle's Contents/Resources so the binary picks them up via
# [NSBundle mainBundle]; elsewhere it's flat next to the executable.
if(APPLE)
    set(MB_RESOURCES_ROOT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/mb.app/Contents/Resources")
else()
    set(MB_RESOURCES_ROOT "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
endif()

# Enable ARC on the .mm files in a source-list variable. Filters by extension
# rather than requiring a separate hand-maintained list — pass the same var
# you handed to add_library/add_executable. No-op on non-Apple. Source-file
# properties are directory-scoped, so call this from the subdir CMakeLists
# that owns the sources, not from here.
function(mb_enable_arc_on_mm sources_var)
    if(NOT APPLE)
        return()
    endif()
    set(_mm ${${sources_var}})
    list(FILTER _mm INCLUDE REGEX "\\.mm$")
    if(_mm)
        set_source_files_properties(${_mm}
            PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    endif()
endfunction()

add_subdirectory(3rdparty)
add_subdirectory(src)
add_subdirectory(tests)

# Convenience target: format every C/C++ file under src/ and tests/ with the
# repo's .clang-format. Run via `ninja clang_format` (or
# `cmake --build build --target clang_format`). Not part of `all` — manual
# only. .mm files are excluded; .clang-format is Language: Cpp.
find_program(CLANG_FORMAT_EXECUTABLE NAMES clang-format)
if(CLANG_FORMAT_EXECUTABLE)
    file(GLOB_RECURSE MB_CLANG_FORMAT_SOURCES
        CONFIGURE_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/*.cpp"
        "${CMAKE_SOURCE_DIR}/src/*.cc"
        "${CMAKE_SOURCE_DIR}/src/*.cxx"
        "${CMAKE_SOURCE_DIR}/src/*.c"
        "${CMAKE_SOURCE_DIR}/src/*.h"
        "${CMAKE_SOURCE_DIR}/src/*.hpp"
        "${CMAKE_SOURCE_DIR}/tests/*.cpp"
        "${CMAKE_SOURCE_DIR}/tests/*.cc"
        "${CMAKE_SOURCE_DIR}/tests/*.cxx"
        "${CMAKE_SOURCE_DIR}/tests/*.c"
        "${CMAKE_SOURCE_DIR}/tests/*.h"
        "${CMAKE_SOURCE_DIR}/tests/*.hpp"
    )
    add_custom_target(clang_format
        COMMAND ${CLANG_FORMAT_EXECUTABLE} -i ${MB_CLANG_FORMAT_SOURCES}
        COMMENT "clang-format src/ and tests/"
        VERBATIM
    )
else()
    message(STATUS "clang-format not found; 'clang_format' target unavailable")
endif()
