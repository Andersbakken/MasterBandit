if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(DAWN_VERSION "v20260409.162024")
    set(DAWN_COMMIT "cbeb98c0887560232bc98c11ba582579f56557f9")
    set(DAWN_REPO "google/dawn")
    set(DAWN_PLATFORM "ubuntu-latest")
    set(DAWN_LIB_DIR "lib64")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    # Patched Dawn: uses presentDrawable: instead of [drawable present] on Metal
    set(DAWN_VERSION "v20260410-presentDrawable-fix")
    set(DAWN_COMMIT "dbecffe5b105a638907d5c88e0973f6f24f91411")
    set(DAWN_REPO "jhanssen/dawn")
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        set(DAWN_PLATFORM "macos-15-intel")
    else()
        set(DAWN_PLATFORM "macos-latest")
    endif()
    set(DAWN_LIB_DIR "lib")
else()
    message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
endif()

set(DAWN_BUILD_TYPE "Release")
set(DAWN_TARBALL_NAME "Dawn-${DAWN_COMMIT}-${DAWN_PLATFORM}-${DAWN_BUILD_TYPE}.tar.gz")
set(DAWN_URL "https://github.com/${DAWN_REPO}/releases/download/${DAWN_VERSION}/${DAWN_TARBALL_NAME}")
set(DAWN_EXTRACT_DIR "${CMAKE_CURRENT_BINARY_DIR}/Dawn-${DAWN_COMMIT}-${DAWN_PLATFORM}-${DAWN_BUILD_TYPE}")

if(NOT EXISTS "${DAWN_EXTRACT_DIR}")
    set(DAWN_TARBALL_PATH "${CMAKE_CURRENT_BINARY_DIR}/${DAWN_TARBALL_NAME}")

    if(NOT EXISTS "${DAWN_TARBALL_PATH}")
        message(STATUS "Downloading Dawn from ${DAWN_URL}")
        file(DOWNLOAD "${DAWN_URL}" "${DAWN_TARBALL_PATH}"
            STATUS DAWN_DOWNLOAD_STATUS
            SHOW_PROGRESS
        )
        list(GET DAWN_DOWNLOAD_STATUS 0 DAWN_DOWNLOAD_ERROR)
        if(NOT DAWN_DOWNLOAD_ERROR EQUAL 0)
            list(GET DAWN_DOWNLOAD_STATUS 1 DAWN_DOWNLOAD_ERROR_MSG)
            file(REMOVE "${DAWN_TARBALL_PATH}")
            message(FATAL_ERROR "Failed to download Dawn: ${DAWN_DOWNLOAD_ERROR_MSG}")
        endif()
    endif()

    message(STATUS "Extracting Dawn")
    file(ARCHIVE_EXTRACT INPUT "${DAWN_TARBALL_PATH}" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
endif()

set(Dawn_DIR "${DAWN_EXTRACT_DIR}/${DAWN_LIB_DIR}/cmake/Dawn" CACHE PATH "Dawn CMake config directory" FORCE)

# Workaround: newer Dawn binary releases leak their entire internal build graph
# into INTERFACE_LINK_LIBRARIES (tint_*, dawn_*, SPIRV-*, absl::*, etc.) even
# though everything is already compiled into libwebgpu_dawn.a. Only system libs
# (pthread, dl, rt) are actually needed. Replace the property entirely and add
# a stub for dawn::dawn_public_config's missing sibling targets so CMake's
# CMP0028 configure-time target validation passes.
set(_dawn_targets_cmake "${DAWN_EXTRACT_DIR}/${DAWN_LIB_DIR}/cmake/Dawn/DawnTargets.cmake")
if(EXISTS "${_dawn_targets_cmake}")
    file(READ "${_dawn_targets_cmake}" _dawn_targets_content)
    if(NOT _dawn_targets_content MATCHES "# patched by MasterBandit build")
        # Replace the bloated INTERFACE_LINK_LIBRARIES with only system deps
        string(REGEX REPLACE
            "set_target_properties\\(dawn::webgpu_dawn PROPERTIES\n  INTERFACE_LINK_LIBRARIES \"[^\"]*\"\n\\)"
            "set_target_properties(dawn::webgpu_dawn PROPERTIES\n  INTERFACE_LINK_LIBRARIES \"dawn::dawn_public_config;\$<LINK_ONLY:Threads::Threads>;\$<LINK_ONLY:dl>;\$<LINK_ONLY:\$<\$<PLATFORM_ID:Linux>:-lrt>>;\$<LINK_ONLY:\$<\$<PLATFORM_ID:Darwin>:-framework IOKit>>;\$<LINK_ONLY:\$<\$<PLATFORM_ID:Darwin>:-framework IOSurface>>\"\n) # patched by MasterBandit build"
            _dawn_targets_content "${_dawn_targets_content}"
        )
        # Remove legacy single-target stub from a previous patch iteration if present
        string(REPLACE
            "# Stub target for partition_alloc (not shipped in binary release)\nadd_library(dawn::partition_alloc INTERFACE IMPORTED)\n\n"
            ""
            _dawn_targets_content "${_dawn_targets_content}"
        )
        file(WRITE "${_dawn_targets_cmake}" "${_dawn_targets_content}")
    endif()
    unset(_dawn_targets_content)
endif()
unset(_dawn_targets_cmake)
