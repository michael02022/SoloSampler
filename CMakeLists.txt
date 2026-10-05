cmake_minimum_required(VERSION 3.16)
project(SoloSampler VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
find_package(X11 REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Threads REQUIRED)

# Icon font (Font Awesome 4, see external/fontawesome/VENDORED_FROM.txt)
# embedded in the binary via xxd -i: the .clap stays a single self-contained
# file, no dependency on a system-installed icon font.
find_program(XXD_EXECUTABLE xxd REQUIRED)
set(FONTAWESOME_DIR ${CMAKE_SOURCE_DIR}/external/fontawesome)
set(FONTAWESOME_DATA_CPP ${CMAKE_BINARY_DIR}/generated/fontawesome_data.cpp)
add_custom_command(
  OUTPUT ${FONTAWESOME_DATA_CPP}
  COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
  COMMAND ${XXD_EXECUTABLE} -i fontawesome-webfont.ttf ${FONTAWESOME_DATA_CPP}
  WORKING_DIRECTORY ${FONTAWESOME_DIR}
  DEPENDS ${FONTAWESOME_DIR}/fontawesome-webfont.ttf
  COMMENT "Embedding fontawesome-webfont.ttf (xxd -i)"
  VERBATIM
)

# sfizz (prebuilt shared library from the sfizioso fork, see lib/include for
# the matching headers - see project memory for how these were verified to
# match the .so's exported symbols).
add_library(sfizz SHARED IMPORTED GLOBAL)
set_target_properties(sfizz PROPERTIES
  IMPORTED_LOCATION             "${CMAKE_CURRENT_SOURCE_DIR}/lib/libsfizz.so.1.2.3"
  IMPORTED_SONAME                "libsfizz.so.1"
  INTERFACE_INCLUDE_DIRECTORIES  "${CMAKE_CURRENT_SOURCE_DIR}/lib/include"
)

add_library(solosampler MODULE
  src/plugin.cpp
  src/gui/gui_window.cpp
  src/gui/editor_ui.cpp
  src/gui/file_dialog.cpp
  src/gui/waveform_view.cpp
  src/gui/zenity_dialog.cpp
  src/sfizz/SfizzEngine.cpp
  src/state/SfzDocument.cpp
  src/state/SfzFlatten.cpp
  src/state/SampleInfo.cpp
  src/state/PresetFile.cpp
  src/midi/ExternalMidi.cpp
  external/imgui/imgui.cpp
  external/imgui/imgui_draw.cpp
  external/imgui/imgui_tables.cpp
  external/imgui/imgui_widgets.cpp
  external/imgui/backends/imgui_impl_opengl2.cpp
  ${FONTAWESOME_DATA_CPP}
)

target_include_directories(solosampler PRIVATE
  src
  external/clap/include
  external/imgui
  external/fontawesome
)

target_link_libraries(solosampler PRIVATE
  sfizz
  PkgConfig::SNDFILE
  X11::X11
  OpenGL::GL
  Threads::Threads
)

set_target_properties(solosampler PROPERTIES
  PREFIX ""
  SUFFIX ".clap"
  OUTPUT_NAME "SoloSampler"
)

# Standalone test host (dlopen's the built .clap, drives it without a real
# DAW) - lets phase-by-phase verification happen headless via screenshot,
# see project memory for why (no synthetic input on the real X11 desktop).
add_executable(mini_host test/mini_host.c)
target_include_directories(mini_host PRIVATE external/clap/include)
target_link_libraries(mini_host PRIVATE ${CMAKE_DL_LIBS} m)
