cmake_minimum_required(VERSION 3.20)

project(ai_control_pc VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/utf-8 /W3)
  # NOMINMAX：避免 windows.h 的 max/min 宏污染 std::max/std::min。
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter -Wno-unused-variable)
endif()

set(RC_CORE_SOURCES
  core/base/log.cpp
  core/base/json.cpp
  core/crypto/hash.cpp
  core/crypto/device_identity.cpp
  core/net/socket.cpp
  core/net/connection.cpp
  core/protocol/protocol.cpp
  core/protocol/video_payload.cpp
  core/session/session.cpp
  core/session/runner.cpp
  core/capture/capture_factory.cpp
  core/codec/codec_factory.cpp
  core/transfer/file_transfer.cpp
)

if(WIN32)
  list(APPEND RC_CORE_SOURCES
    core/capture/dxgi_capture.cpp
    core/clipboard/clipboard_win.cpp
    core/input/windows_input_injector.cpp
    core/codec/jpeg_encoder.cpp
    core/codec/h264_mf_encoder.cpp
    core/codec/h264_decoder.cpp
    core/codec/d3d11_video_converter.cpp
    core/codec/color_convert.cpp
    core/render/window_renderer.cpp
  )
endif()

if(UNIX AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(X11 REQUIRED x11)
  pkg_check_modules(XEXT REQUIRED xext)
  pkg_check_modules(LIBAVCODEC REQUIRED libavcodec)
  pkg_check_modules(LIBAVUTIL REQUIRED libavutil)
  pkg_check_modules(LIBSWSCALE REQUIRED libswscale)
  pkg_check_modules(LIBAVFORMAT REQUIRED libavformat)
  pkg_check_modules(SDL2 REQUIRED sdl2)

  list(APPEND RC_CORE_SOURCES
    core/capture/x11_capture.cpp
    core/clipboard/clipboard_linux.cpp
    core/input/linux_input_injector.cpp
    core/codec/ffmpeg_jpeg_encoder.cpp
    core/codec/ffmpeg_h264_encoder.cpp
    core/codec/ffmpeg_h264_decoder.cpp
    core/render/sdl_renderer.cpp
  )

  pkg_check_modules(PIPEWIRE libpipewire-0.3)
  pkg_check_modules(GLIB glib-2.0)
  pkg_check_modules(GIO gio-2.0)
  pkg_check_modules(LIBPORTAL libportal)
  if(PIPEWIRE_FOUND AND GLIB_FOUND AND GIO_FOUND AND LIBPORTAL_FOUND)
    list(APPEND RC_CORE_SOURCES core/capture/pipewire_capture.cpp)
    set(HAVE_PIPEWIRE_CAPTURE ON)
    message(STATUS "PipeWire ScreenCast capture enabled (portal+pw)")
  else()
    message(STATUS "PipeWire deps not found, pipewire capture disabled")
  endif()
endif()

add_library(rc_core STATIC ${RC_CORE_SOURCES})

target_include_directories(rc_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/core
  ${CMAKE_CURRENT_SOURCE_DIR}/proto
)

if(WIN32)
  target_link_libraries(rc_core PUBLIC
    ws2_32 d3d11 dxgi ole32 windowscodecs mfplat mfuuid mf user32)
endif()

if(UNIX AND NOT APPLE)
  target_include_directories(rc_core PUBLIC
    ${X11_INCLUDE_DIRS}
    ${XEXT_INCLUDE_DIRS}
    ${LIBAVCODEC_INCLUDE_DIRS}
    ${LIBAVUTIL_INCLUDE_DIRS}
    ${LIBSWSCALE_INCLUDE_DIRS}
    ${LIBAVFORMAT_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
  )
  target_link_libraries(rc_core PUBLIC
    ${X11_LIBRARIES}
    ${XEXT_LIBRARIES}
    ${LIBAVCODEC_LIBRARIES}
    ${LIBAVUTIL_LIBRARIES}
    ${LIBSWSCALE_LIBRARIES}
    ${LIBAVFORMAT_LIBRARIES}
    ${SDL2_LIBRARIES}
    dl pthread
  )
  target_link_directories(rc_core PUBLIC
    ${X11_LIBRARY_DIRS}
    ${XEXT_LIBRARY_DIRS}
    ${LIBAVCODEC_LIBRARY_DIRS}
    ${LIBAVUTIL_LIBRARY_DIRS}
    ${LIBSWSCALE_LIBRARY_DIRS}
    ${LIBAVFORMAT_LIBRARY_DIRS}
    ${SDL2_LIBRARY_DIRS}
  )
  if(HAVE_PIPEWIRE_CAPTURE)
    target_include_directories(rc_core PUBLIC
      ${PIPEWIRE_INCLUDE_DIRS} ${GLIB_INCLUDE_DIRS} ${GIO_INCLUDE_DIRS}
      ${LIBPORTAL_INCLUDE_DIRS})
    target_link_libraries(rc_core PUBLIC
      ${PIPEWIRE_LIBRARIES} ${GLIB_LIBRARIES} ${GIO_LIBRARIES}
      ${LIBPORTAL_LIBRARIES})
    target_link_directories(rc_core PUBLIC
      ${PIPEWIRE_LIBRARY_DIRS} ${GLIB_LIBRARY_DIRS} ${GIO_LIBRARY_DIRS}
      ${LIBPORTAL_LIBRARY_DIRS})
    target_compile_definitions(rc_core PUBLIC HAVE_PIPEWIRE_CAPTURE=1)
  endif()
endif()

# 控制通道 TLS（中继 <-> 被控端/主控端）：mbed TLS 可选依赖。
# 关闭后仍可用明文 TCP，但公网部署不建议。
option(RC_ENABLE_TLS "Enable TLS for relay control channel (mbed TLS)" ON)
if(RC_ENABLE_TLS)
  if(WIN32)
    set(RC_MBEDTLS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls-install")
    if(NOT EXISTS "${RC_MBEDTLS_ROOT}/include/mbedtls/ssl.h")
      message(FATAL_ERROR "缺少 third_party/mbedtls-install（可用 -DRC_ENABLE_TLS=OFF 禁用）")
    endif()
    target_include_directories(rc_core PUBLIC "${RC_MBEDTLS_ROOT}/include")
    target_link_libraries(rc_core PUBLIC
      "${RC_MBEDTLS_ROOT}/lib/mbedtls.lib"
      "${RC_MBEDTLS_ROOT}/lib/mbedx509.lib"
      "${RC_MBEDTLS_ROOT}/lib/mbedcrypto.lib"
      "${RC_MBEDTLS_ROOT}/lib/everest.lib"
      "${RC_MBEDTLS_ROOT}/lib/p256m.lib"
      bcrypt)
  else()
    pkg_check_modules(MBEDTLS mbedtls)
    if(NOT MBEDTLS_FOUND)
      message(FATAL_ERROR "未找到 mbedtls（可用 -DRC_ENABLE_TLS=OFF 禁用）")
    endif()
    target_include_directories(rc_core PUBLIC ${MBEDTLS_INCLUDE_DIRS})
    target_link_libraries(rc_core PUBLIC ${MBEDTLS_LIBRARIES})
    target_link_directories(rc_core PUBLIC ${MBEDTLS_LIBRARY_DIRS})
  endif()
  target_compile_definitions(rc_core PUBLIC RC_ENABLE_TLS=1)
else()
  target_compile_definitions(rc_core PUBLIC RC_ENABLE_TLS=0)
endif()

add_executable(rc-relay server/relay/main.cpp)
target_link_libraries(rc-relay PRIVATE rc_core)

# Web 预览/控制网关：复用 ClientRunner 的会话与解码，向浏览器输出 MJPEG。
add_executable(rc-web
  server/web/main.cpp
  server/web/http_server.cpp
  server/web/web_page.cpp
)
target_link_libraries(rc-web PRIVATE rc_core)

add_executable(rc-cli apps/cli/main.cpp)
if(WIN32)
  target_sources(rc-cli PRIVATE core/render/window_renderer.cpp)
  target_link_libraries(rc-cli PRIVATE gdi32 user32)
endif()
if(UNIX AND NOT APPLE)
  target_sources(rc-cli PRIVATE core/render/sdl_renderer.cpp)
endif()
target_link_libraries(rc-cli PRIVATE rc_core)

# 桌面 UI 用 Qt：优先 Qt6，找不到再回退 Qt5；两平台共用同一套界面代码与样式。
option(ENABLE_QT "Build Qt GUI app (rc-app)" ON)
if(ENABLE_QT)
  find_package(Qt6 QUIET COMPONENTS Core Gui Widgets)
  if(Qt6_FOUND)
    set(RC_QT_PREFIX Qt6)
  else()
    find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets)
    set(RC_QT_PREFIX Qt5)
  endif()

  add_executable(rc-app
    apps/qt/main.cpp
    apps/qt/main_window.cpp
    apps/qt/backend_worker.cpp
    apps/qt/resources.qrc
  )
  set_target_properties(rc-app PROPERTIES
    AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
  target_include_directories(rc-app PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/apps/qt
  )
  target_link_libraries(rc-app PRIVATE
    rc_core
    ${RC_QT_PREFIX}::Core ${RC_QT_PREFIX}::Gui ${RC_QT_PREFIX}::Widgets
  )
  if(WIN32)
    # GUI 子系统，避免额外弹出控制台窗口。
    set_target_properties(rc-app PROPERTIES WIN32_EXECUTABLE ON)
  endif()
endif()

# WebRTC 传输（可选）：libdatachannel 承载 RTP/SCTP，信令复用现有中继。
# 依赖需先手动准备到 third_party（见 res/WEBRTC-POC.md）。
option(RC_ENABLE_WEBRTC_POC "Build WebRTC transport POC (libdatachannel)" OFF)
option(RC_ENABLE_WEBRTC "Enable WebRTC transport in rc_core (libdatachannel)" OFF)
if(RC_ENABLE_WEBRTC_POC OR RC_ENABLE_WEBRTC)
  set(RC_LDC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libdatachannel-0.24.6")
  set(RC_MBEDTLS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls-install")
  if(NOT EXISTS "${RC_LDC_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "缺少 third_party/libdatachannel-0.24.6")
  endif()
  if(NOT EXISTS "${RC_MBEDTLS_ROOT}/lib/mbedcrypto.lib")
    message(FATAL_ERROR "缺少 third_party/mbedtls-install（先构建并安装 Mbed TLS）")
  endif()

  # libdatachannel 的 FindMbedTLS.cmake 只认这几个缓存变量，先手工喂进去。
  set(MbedTLS_INCLUDE_DIR "${RC_MBEDTLS_ROOT}/include" CACHE PATH "" FORCE)
  set(MbedTLS_LIBRARY "${RC_MBEDTLS_ROOT}/lib/mbedtls.lib" CACHE FILEPATH "" FORCE)
  set(MbedX509_LIBRARY "${RC_MBEDTLS_ROOT}/lib/mbedx509.lib" CACHE FILEPATH "" FORCE)
  set(MbedCrypto_LIBRARY "${RC_MBEDTLS_ROOT}/lib/mbedcrypto.lib" CACHE FILEPATH "" FORCE)

  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(NO_EXAMPLES ON CACHE BOOL "" FORCE)
  set(NO_TESTS ON CACHE BOOL "" FORCE)
  set(NO_WEBSOCKET ON CACHE BOOL "" FORCE)
  set(USE_MBEDTLS ON CACHE BOOL "" FORCE)
  add_subdirectory(${RC_LDC_DIR} ${CMAKE_BINARY_DIR}/libdatachannel EXCLUDE_FROM_ALL)

  # Mbed TLS 3.6 的 mbedcrypto 依赖 everest/p256m 两个静态库。
  foreach(rc_lib datachannel-static datachannel)
    if(TARGET ${rc_lib})
      target_link_libraries(${rc_lib} PRIVATE
        "${RC_MBEDTLS_ROOT}/lib/everest.lib" "${RC_MBEDTLS_ROOT}/lib/p256m.lib")
    endif()
  endforeach()
endif()

if(RC_ENABLE_WEBRTC)
  target_sources(rc_core PRIVATE core/webrtc/webrtc_transport.cpp)
  target_link_libraries(rc_core PUBLIC datachannel-static)
  target_compile_definitions(rc_core PUBLIC RC_ENABLE_WEBRTC=1)
  if(WIN32)
    target_link_libraries(rc_core PUBLIC ws2_32 secur32 crypt32)
  endif()
endif()

if(RC_ENABLE_WEBRTC_POC)
  add_executable(rc-webrtc-poc apps/poc/webrtc_poc.cpp)
  set_target_properties(rc-webrtc-poc PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
  target_link_libraries(rc-webrtc-poc PRIVATE rc_core datachannel-static)
  if(WIN32)
    target_link_libraries(rc-webrtc-poc PRIVATE ws2_32 secur32 crypt32)
  endif()
endif()

