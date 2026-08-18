cmake_minimum_required(VERSION 3.20)
project(linky_sender LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

find_package(PkgConfig REQUIRED)
pkg_check_modules(AV   REQUIRED IMPORTED_TARGET libavcodec libavformat libavutil libswscale libswresample libavdevice)
pkg_check_modules(PW   REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(AVA  REQUIRED IMPORTED_TARGET avahi-client)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0)
pkg_check_modules(OPENSSL REQUIRED IMPORTED_TARGET openssl)

option(LINKY_GTK "Build the GTK4 UI" ON)
if(LINKY_GTK)
  pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
endif()

# Protocolo zwlr_screencopy (vendido de wlr-protocols, CC0): el paquete del
# sistema no trae headers; se genera con wayland-scanner.
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
set(SCREENCOPY_XML ${CMAKE_CURRENT_SOURCE_DIR}/third_party/wlr-screencopy-unstable-v1.xml)
set(SCREENCOPY_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/wlr-screencopy-unstable-v1-client-protocol.h)
set(SCREENCOPY_CODE ${CMAKE_CURRENT_BINARY_DIR}/generated/wlr-screencopy-unstable-v1.c)
add_custom_command(
  OUTPUT ${SCREENCOPY_HEADER}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
  COMMAND ${WAYLAND_SCANNER} client-header ${SCREENCOPY_XML} ${SCREENCOPY_HEADER}
  DEPENDS ${SCREENCOPY_XML}
  COMMENT "Generando wlr-screencopy-unstable-v1 client header"
)
add_custom_command(
  OUTPUT ${SCREENCOPY_CODE}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/generated
  COMMAND ${WAYLAND_SCANNER} private-code ${SCREENCOPY_XML} ${SCREENCOPY_CODE}
  DEPENDS ${SCREENCOPY_XML}
  COMMENT "Generando wlr-screencopy-unstable-v1 code"
)
add_custom_target(screencopy_proto DEPENDS ${SCREENCOPY_HEADER} ${SCREENCOPY_CODE})

add_executable(linky-sender
  src/main.cpp
  src/common/log.cpp
  src/common/json.cpp
  src/common/util.cpp
  src/discovery/discovery.cpp
  src/capture/video_portal.cpp
  src/capture/video_screencopy.cpp
  ${SCREENCOPY_HEADER}
  ${SCREENCOPY_CODE}
  src/capture/video_x11.cpp
  src/capture/audio.cpp
  src/encode/encoder.cpp
  src/rtp/packetizer.cpp
  src/rtp/rtcp.cpp
  src/session/control.cpp
  src/app/sender_app.cpp
)
target_include_directories(linky-sender PRIVATE src ${CMAKE_CURRENT_BINARY_DIR}/generated)
add_dependencies(linky-sender screencopy_proto)
target_link_libraries(linky-sender PRIVATE
  PkgConfig::AV PkgConfig::PW PkgConfig::AVA PkgConfig::WAYLAND
  PkgConfig::GLIB PkgConfig::OPENSSL
)
if(LINKY_GTK)
  target_sources(linky-sender PRIVATE src/ui/gtk_app.cpp)
  target_compile_definitions(linky-sender PRIVATE LINKY_HAS_GTK=1)
  target_link_libraries(linky-sender PRIVATE PkgConfig::GTK4)
endif()

add_executable(linky-dumpreceiver
  tools/dumpreceiver/main.cpp
  src/common/log.cpp
  src/common/json.cpp
  src/common/util.cpp
)
target_include_directories(linky-dumpreceiver PRIVATE src)
target_link_libraries(linky-dumpreceiver PRIVATE PkgConfig::AV PkgConfig::OPENSSL)
