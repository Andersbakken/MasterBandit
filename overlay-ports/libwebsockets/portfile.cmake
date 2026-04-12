vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO warmcat/libwebsockets
    REF "v${VERSION}"
    SHA512 77fcb15c325d514fee18193a6509755618ce4232115259377d67f93015490a11642f5974fddd2efebc89496e28a52f0a135b6635c662be1e2c641aaa68397b11
    HEAD_REF master
    PATCHES
        fix-build-error.patch
        export-include-path.patch
)

string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "static" LWS_WITH_STATIC)
string(COMPARE EQUAL "${VCPKG_LIBRARY_LINKAGE}" "dynamic" LWS_WITH_SHARED)
string(COMPARE EQUAL "${VCPKG_CRT_LINKAGE}" "static" STATIC_CRT)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DLWS_WITH_STATIC=${LWS_WITH_STATIC}
        -DLWS_WITH_SHARED=${LWS_WITH_SHARED}
        -DLWS_MSVC_STATIC_RUNTIME=${STATIC_CRT}
        -DLWS_WITH_GENCRYPTO=ON
        -DLWS_WITH_TLS=ON
        -DLWS_WITH_BUNDLED_ZLIB=OFF
        -DLWS_WITHOUT_TESTAPPS=ON
        -DLWS_IPV6=ON
        -DLWS_WITH_HTTP2=ON
        -DLWS_WITH_HTTP_STREAM_COMPRESSION=ON
        -DLWS_WITH_EXTERNAL_POLL=ON
        # Event libraries — we drive lws via external poll integrated with our EventLoop
        -DLWS_WITH_LIBUV=OFF
        -DLWS_WITH_LIBEV=OFF
        -DLWS_WITH_LIBEVENT=OFF
        # Server-framework features we don't use
        -DLWS_WITH_LWSWS=OFF
        -DLWS_WITH_CGI=OFF
        -DLWS_WITH_HTTP_PROXY=OFF
        -DLWS_WITH_SOCKS5=OFF
        -DLWS_WITH_ACCESS_LOG=OFF
        -DLWS_WITH_RANGES=OFF
        -DLWS_WITH_SERVER_STATUS=OFF
        -DLWS_WITH_GENERIC_SESSIONS=OFF
        -DLWS_WITH_PEER_LIMITS=OFF
        -DLWS_WITH_ACME=OFF
        -DLWS_WITH_PLUGINS=OFF
        -DLWS_WITH_HUBBUB=OFF
        -DLWS_WITH_FTS=OFF
        -DLWS_WITH_THREADPOOL=OFF
        -DLWS_WITH_SMTP=OFF
        -DLWS_WITH_SQLITE3=OFF
        -DLWS_WITH_JOSE=OFF
        -DLWS_ROLE_DBUS=OFF
        -DLWS_ROLE_RAW_PROXY=OFF
        -DLWS_WITH_ZIP_FOPS=OFF
        -DLWS_WITH_DISKCACHE=OFF
        -DLWS_WITH_LWS_DSH=OFF
        -DLWS_WITH_MINIMAL_EXAMPLES=OFF
        -DLWS_WITH_LATENCY=OFF
        -DLWS_WITH_STATS=OFF
        -DLWS_WITH_SELFTESTS=OFF
)

vcpkg_cmake_install()

if (VCPKG_TARGET_IS_WINDOWS)
    vcpkg_cmake_config_fixup(CONFIG_PATH cmake)
else()
    vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/libwebsockets)
endif()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/share")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/share/libwebsockets-test-server")
file(READ "${CURRENT_PACKAGES_DIR}/share/libwebsockets/libwebsockets-config.cmake" LIBWEBSOCKETSCONFIG_CMAKE)
string(REPLACE "/../include" "/../../include" LIBWEBSOCKETSCONFIG_CMAKE "${LIBWEBSOCKETSCONFIG_CMAKE}")
file(WRITE "${CURRENT_PACKAGES_DIR}/share/libwebsockets/libwebsockets-config.cmake" "${LIBWEBSOCKETSCONFIG_CMAKE}")

if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "debug")
    vcpkg_replace_string( "${CURRENT_PACKAGES_DIR}/share/libwebsockets/LibwebsocketsTargets-debug.cmake" "websockets_static.lib" "websockets.lib" IGNORE_UNCHANGED)
endif()

if(NOT DEFINED VCPKG_BUILD_TYPE OR VCPKG_BUILD_TYPE STREQUAL "release")
    vcpkg_replace_string( "${CURRENT_PACKAGES_DIR}/share/libwebsockets/LibwebsocketsTargets-release.cmake" "websockets_static.lib" "websockets.lib" IGNORE_UNCHANGED)
endif()

if (VCPKG_LIBRARY_LINKAGE STREQUAL static)
    if (VCPKG_TARGET_IS_WINDOWS)
        file(RENAME "${CURRENT_PACKAGES_DIR}/debug/lib/websockets_static.lib" "${CURRENT_PACKAGES_DIR}/debug/lib/websockets.lib")
        file(RENAME "${CURRENT_PACKAGES_DIR}/lib/websockets_static.lib" "${CURRENT_PACKAGES_DIR}/lib/websockets.lib")
    endif()
endif ()

vcpkg_copy_pdbs()
vcpkg_fixup_pkgconfig()
vcpkg_replace_string("${CURRENT_PACKAGES_DIR}/include/lws_config.h" "${CURRENT_PACKAGES_DIR}" "")

file(INSTALL "${SOURCE_PATH}/LICENSE" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}" RENAME copyright)
