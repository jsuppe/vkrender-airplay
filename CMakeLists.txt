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
set(GST_ROOT "" CACHE PATH "GStreamer MSVC x86_64 root")
if(NOT GST_ROOT)
    foreach(_cand "C:/gstreamer/1.0/msvc_x86_64"
                  "C:/Program Files/gstreamer/1.0/msvc_x86_64")
        if(NOT GST_ROOT AND EXISTS "${_cand}/lib/gstreamer-1.0.lib")
            set(GST_ROOT "${_cand}")
        endif()
    endforeach()
endif()
if(NOT EXISTS "${GST_ROOT}/lib/gstreamer-1.0.lib")
    message(FATAL_ERROR "GStreamer MSVC not found - install the runtime + devel "
                        "MSIs and/or pass -DGST_ROOT=...")
endif()
add_library(GStreamer::win INTERFACE IMPORTED)
target_include_directories(GStreamer::win INTERFACE
    "${GST_ROOT}/include/gstreamer-1.0"
    "${GST_ROOT}/include/glib-2.0"
    "${GST_ROOT}/lib/glib-2.0/include"
    "${GST_ROOT}/lib/gstreamer-1.0/include")
target_link_directories(GStreamer::win INTERFACE "${GST_ROOT}/lib")
target_link_libraries(GStreamer::win INTERFACE
    gstreamer-1.0.lib gstapp-1.0.lib gstvideo-1.0.lib gobject-2.0.lib glib-2.0.lib)

# ---- framelink ---------------------------------------------------------------
# This repo no longer depends on vkrender at all. It is a framelink producer
# (github: framelink, MIT), which is a published library rather than another
# application's client DLL - so this receiver can feed anything that creates
# framelink input slots, not only this one renderer.
# A snapshot is vendored under sdk/ so the repo builds standalone.
set(FRAMELINK_SDK_DIR "${CMAKE_SOURCE_DIR}/sdk" CACHE PATH
    "Directory containing framelink.h/.lib/.dll")

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
target_include_directories(vkr_airplay PRIVATE ${UX} "${FRAMELINK_SDK_DIR}"
    "${GST_ROOT}/include")
target_link_libraries(vkr_airplay PRIVATE
    raop_static GStreamer::win d3d11 dxgi
    "${FRAMELINK_SDK_DIR}/framelink.lib")
target_compile_options(vkr_airplay PRIVATE /EHsc)
add_custom_command(TARGET vkr_airplay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${FRAMELINK_SDK_DIR}/framelink.dll" $<TARGET_FILE_DIR:vkr_airplay>)
