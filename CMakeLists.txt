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
  core/net/socket.cpp
  core/net/connection.cpp
  core/protocol/protocol.cpp
  core/protocol/video_payload.cpp
  core/session/session.cpp
  core/session/runner.cpp
  core/capture/capture_factory.cpp
  core/codec/codec_factory.cpp
)

if(WIN32)
  list(APPEND RC_CORE_SOURCES
    core/capture/dxgi_capture.cpp
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
    core/codec/ffmpeg_jpeg_encoder.cpp
    core/codec/ffmpeg_h264_encoder.cpp
    core/codec/ffmpeg_h264_decoder.cpp
    core/render/sdl_renderer.cpp
  )
endif()

add_library(rc_core STATIC ${RC_CORE_SOURCES})

target_include_directories(rc_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/core
  ${CMAKE_CURRENT_SOURCE_DIR}/proto
)

if(WIN32)
  target_link_libraries(rc_core PUBLIC
    ws2_32 d3d11 dxgi ole32 windowscodecs mfplat mfuuid mf)
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
endif()

add_executable(rc-relay server/relay/main.cpp)
target_link_libraries(rc-relay PRIVATE rc_core)

add_executable(rc-cli apps/cli/main.cpp)
if(WIN32)
  target_sources(rc-cli PRIVATE core/render/window_renderer.cpp)
  target_link_libraries(rc-cli PRIVATE gdi32 user32)
endif()
if(UNIX AND NOT APPLE)
  target_sources(rc-cli PRIVATE core/render/sdl_renderer.cpp)
endif()
target_link_libraries(rc-cli PRIVATE rc_core)

# 桌面 UI 统一用 Qt5：Windows / Linux 共用同一套界面代码与样式。
option(ENABLE_QT "Build Qt5 GUI app (rc-app)" ON)
if(ENABLE_QT)
  find_package(Qt5 REQUIRED COMPONENTS Core Gui Widgets)

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
    Qt5::Core Qt5::Gui Qt5::Widgets
  )
  if(WIN32)
    # GUI 子系统，避免额外弹出控制台窗口。
    set_target_properties(rc-app PROPERTIES WIN32_EXECUTABLE ON)
  endif()
endif()
