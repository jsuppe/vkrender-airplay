cmake_minimum_required(VERSION 3.20)
project(vkrender_airplay LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This receiver targets Windows (MSVC). The same UxPlay "
                        "core runs on Android inside the vkrender renderer project.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- GStreamer (decode) + OpenSSL (pairing crypto, bundled with GStreamer) --
# Install the MSVC 1.0 runtime + development MSIs from
# https://gstreamer.freedesktop.org/download/ .
# The devel MSI's environment variable is tried first but only trusted when it
# actually points at an SDK (a stale or mangled value must not break the build).
set(GST_ROOT "" CACHE PATH "GStreamer MSVC x86_64 root")
if(NOT GST_ROOT)
    foreach(_cand "$ENV{GSTREAMER_1_0_ROOT_MSVC_X86_64}"
                  "C:/gstreamer/1.0/msvc_x86_64"
                  "C:/Program Files/gstreamer/1.0/msvc_x86_64")
        if(NOT GST_ROOT AND NOT "${_cand}" STREQUAL "" AND EXISTS "${_cand}/lib/gstreamer-1.0.lib")
            set(GST_ROOT "${_cand}")
        endif()
    endforeach()
endif()
if(NOT EXISTS "${GST_ROOT}/lib/gstreamer-1.0.lib")
    message(FATAL_ERROR "GStreamer MSVC not found - install the runtime + devel "
                        "MSIs and/or pass -DGST_ROOT=...")
endif()
# The zero-copy decode path drives d3d11h264dec and GstD3D11Converter through
# GStreamer's D3D11 library (gst-plugins-bad, API 1.22+). No silent fallback to
# a CPU-only build: a receiver that quietly uploads every frame is not this one.
if(NOT EXISTS "${GST_ROOT}/lib/gstd3d11-1.0.lib")
    message(FATAL_ERROR "gstd3d11-1.0.lib not found under '${GST_ROOT}/lib' - the "
                        "GPU decode path needs the GStreamer MSVC development "
                        "package 1.22 or newer (1.26.x tested).")
endif()
add_library(GStreamer::win INTERFACE IMPORTED)
target_include_directories(GStreamer::win INTERFACE
    "${GST_ROOT}/include/gstreamer-1.0"
    "${GST_ROOT}/include/glib-2.0"
    "${GST_ROOT}/lib/glib-2.0/include"
    "${GST_ROOT}/lib/gstreamer-1.0/include")
target_link_directories(GStreamer::win INTERFACE "${GST_ROOT}/lib")
target_link_libraries(GStreamer::win INTERFACE
    gstreamer-1.0.lib gstapp-1.0.lib gstvideo-1.0.lib gstd3d11-1.0.lib
    gobject-2.0.lib glib-2.0.lib)
# gst/d3d11 is an unstable-API library: opt in explicitly.
target_compile_definitions(GStreamer::win INTERFACE GST_USE_UNSTABLE_API)

# ---- framelink ---------------------------------------------------------------
# This receiver is a framelink producer (github: jsuppe/framelink, MIT): it
# feeds anything that creates framelink input slots, not only vkrender. The
# library is built from source at ONE pinned revision, so the repo builds
# standalone and producer and renderer agree on FL_VERSION (the wire format is
# refused at handshake otherwise). Move FRAMELINK_GIT_TAG to update it, or
# point FRAMELINK_DIR at a local checkout, as vkrender does.
set(FRAMELINK_GIT_TAG "3ba77d4a3d493d51c622397b7b5ff3ead4bddf6d" CACHE STRING
    "framelink revision to fetch and build (master, 2026-11-03)")
set(FRAMELINK_DIR "" CACHE PATH
    "Build this framelink checkout instead of fetching FRAMELINK_GIT_TAG")
# Library only: no self-tests, no samples, and no GStreamer plugin - the
# receiver talks to the ring through the C API and to the decoder through
# gst/d3d11 directly, so gstframelink.dll is not needed at runtime.
set(FRAMELINK_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(FRAMELINK_BUILD_SAMPLES OFF CACHE BOOL "" FORCE)
set(FRAMELINK_BUILD_GST OFF CACHE BOOL "" FORCE)
if(FRAMELINK_DIR)
    if(NOT EXISTS "${FRAMELINK_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "framelink not found at '${FRAMELINK_DIR}'")
    endif()
    add_subdirectory("${FRAMELINK_DIR}" "${CMAKE_BINARY_DIR}/framelink")
else()
    include(FetchContent)
    FetchContent_Declare(framelink
        GIT_REPOSITORY https://github.com/jsuppe/framelink.git
        GIT_TAG        ${FRAMELINK_GIT_TAG})
    FetchContent_MakeAvailable(framelink)
endif()

# ---- vendored libplist (LGPL-2.1) ------------------------------------------
set(PLIST ${CMAKE_SOURCE_DIR}/third_party/libplist)
add_library(plist_static STATIC
    ${PLIST}/src/base64.c ${PLIST}/src/bytearray.c ${PLIST}/src/bplist.c
    ${PLIST}/src/hashtable.c ${PLIST}/src/jplist.c ${PLIST}/src/jsmn.c
    ${PLIST}/src/oplist.c ${PLIST}/src/plist.c ${PLIST}/src/ptrarray.c
    ${PLIST}/src/time64.c ${PLIST}/src/xplist.c
    ${PLIST}/src/out-default.c ${PLIST}/src/out-limd.c ${PLIST}/src/out-plutil.c
    ${PLIST}/libcnary/cnary.c ${PLIST}/libcnary/node.c ${PLIST}/libcnary/node_list.c)
target_include_directories(plist_static PUBLIC
    ${PLIST}/include ${PLIST}/src ${PLIST}/libcnary/include)
target_compile_definitions(plist_static PUBLIC LIBPLIST_STATIC=1)
target_compile_definitions(plist_static PRIVATE
    PACKAGE_VERSION="2.6.0" WIN32=1 _CRT_SECURE_NO_WARNINGS)

# ---- vendored UxPlay RAOP/mirroring core (GPL-3.0) + Win32 shims ------------
set(UX ${CMAKE_SOURCE_DIR}/third_party/uxplay/lib)
set(WINCOMPAT ${CMAKE_SOURCE_DIR}/third_party/win_compat)
add_library(raop_static STATIC
    ${UX}/byteutils.c ${UX}/crypto.c ${UX}/fairplay_playfair.c
    ${UX}/http_request.c ${UX}/http_response.c ${UX}/httpd.c ${UX}/logger.c
    ${UX}/mirror_buffer.c ${UX}/netutils.c ${UX}/pairing.c ${UX}/raop.c
    ${UX}/raop_buffer.c ${UX}/raop_ntp.c ${UX}/raop_rtp.c
    ${UX}/raop_rtp_mirror.c ${UX}/srp.c ${UX}/utils.c
    ${UX}/llhttp/api.c ${UX}/llhttp/http.c ${UX}/llhttp/llhttp.c
    ${UX}/playfair/hand_garble.c ${UX}/playfair/modified_md5.c
    ${UX}/playfair/omg_hax.c ${UX}/playfair/playfair.c ${UX}/playfair/sap_hash.c
    src/dnssd_win.c
    ${WINCOMPAT}/pthread.c)
# win_compat first so <pthread.h>/<unistd.h> resolve to the shim.
target_include_directories(raop_static PUBLIC
    ${WINCOMPAT} ${UX} ${UX}/llhttp ${UX}/playfair
    "${GST_ROOT}/include")  # openssl headers ship with GStreamer MSVC
target_link_directories(raop_static PUBLIC "${GST_ROOT}/lib")
target_link_libraries(raop_static PUBLIC plist_static
    crypto.lib ssl.lib dnsapi ws2_32 iphlpapi)
target_compile_definitions(raop_static PRIVATE WIN32=1 _CRT_SECURE_NO_WARNINGS)
target_compile_options(raop_static PRIVATE /w) # third-party C stays quiet

# ---- the receiver -----------------------------------------------------------
# OpenSSL FILE* BIOs cross the DLL boundary -> applink.c must live in the exe.
add_executable(vkr_airplay src/airplay_win.cpp src/pin_overlay.cpp
    "${GST_ROOT}/include/openssl/applink.c")
target_include_directories(vkr_airplay PRIVATE ${UX} "${GST_ROOT}/include")
target_link_libraries(vkr_airplay PRIVATE
    raop_static GStreamer::win framelink d3d11 dxgi)
target_compile_options(vkr_airplay PRIVATE /EHsc)
# framelink.dll is built in its own sub-directory; put it beside the exe so a
# plain `vkr_airplay.exe` run (and the CI artifact) finds it.
add_custom_command(TARGET vkr_airplay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:framelink> $<TARGET_FILE_DIR:vkr_airplay>)
