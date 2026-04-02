set(DAWN_VERSION "v20260219.200501")
set(DAWN_COMMIT "536c572abab2602db0b652eda401ebd01e046c11")

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(DAWN_PLATFORM "ubuntu-latest")
    set(DAWN_LIB_DIR "lib64")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
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
set(DAWN_URL "https://github.com/google/dawn/releases/download/${DAWN_VERSION}/${DAWN_TARBALL_NAME}")
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
