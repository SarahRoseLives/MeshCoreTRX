cmake_minimum_required(VERSION 3.20)
project(MeshCoreTRX C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_library(IIO_LIBRARY NAMES iio REQUIRED)
find_path(IIO_INCLUDE_DIR NAMES iio.h REQUIRED)

add_library(mctrx_core STATIC
    src/phy/lora_mod.cpp
    src/phy/lora_demod.cpp
    src/mesh/meshcore.cpp
    src/mesh/meshcore_build.cpp
    src/mesh/identity.cpp
    src/app/engine.cpp
    src/app/radio.cpp
    src/io/wav_writer.cpp
    src/sdr/pluto_tx.cpp
    src/sdr/pluto_radio.cpp
    src/sdr/decimator.cpp
    src/third_party/ed25519/fe.c
    src/third_party/ed25519/ge.c
    src/third_party/ed25519/key_exchange.c
    src/third_party/ed25519/keypair.c
    src/third_party/ed25519/sc.c
    src/third_party/ed25519/seed.c
    src/third_party/ed25519/sha512.c
    src/third_party/ed25519/sign.c
    src/third_party/ed25519/verify.c
)
target_include_directories(mctrx_core PUBLIC
    src
    src/third_party/ed25519
    ${IIO_INCLUDE_DIR}
)
target_link_libraries(mctrx_core PUBLIC ${IIO_LIBRARY})

add_executable(MeshCoreTRX-cli src/main_cli.cpp)
target_link_libraries(MeshCoreTRX-cli PRIVATE mctrx_core)

add_executable(MeshCoreTRX-rxtest src/rx_test.cpp)
target_link_libraries(MeshCoreTRX-rxtest PRIVATE mctrx_core)

add_executable(MeshCoreTRX-radiotest src/radio_test.cpp)
target_link_libraries(MeshCoreTRX-radiotest PRIVATE mctrx_core)

add_executable(MeshCoreTRX-prototest src/proto_test.cpp)
target_link_libraries(MeshCoreTRX-prototest PRIVATE mctrx_core)

add_executable(MeshCoreTRX-persisttest src/persistence_test.cpp)
target_link_libraries(MeshCoreTRX-persisttest PRIVATE mctrx_core)

option(MCTRX_BUILD_GUI "Build the Dear ImGui application" OFF)
if(MCTRX_BUILD_GUI AND WIN32)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLFW REQUIRED IMPORTED_TARGET glfw3)

  add_library(mctrx_imgui STATIC
      src/third_party/imgui/imgui.cpp
      src/third_party/imgui/imgui_draw.cpp
      src/third_party/imgui/imgui_tables.cpp
      src/third_party/imgui/imgui_widgets.cpp
      src/third_party/imgui/backends/imgui_impl_glfw.cpp
      src/third_party/imgui/backends/imgui_impl_opengl3.cpp
  )
  target_include_directories(mctrx_imgui PUBLIC
      src/third_party/imgui
      src/third_party/imgui/backends
  )
  target_link_libraries(mctrx_imgui PUBLIC PkgConfig::GLFW opengl32)

  add_executable(MeshCoreTRX WIN32
      src/main_gui.cpp
      src/ui/ui.cpp
  )
  target_include_directories(MeshCoreTRX PRIVATE src src/third_party/ed25519 src/ui ${IIO_INCLUDE_DIR})
  target_link_libraries(MeshCoreTRX PRIVATE mctrx_core mctrx_imgui ${IIO_LIBRARY} opengl32)
  set_target_properties(MeshCoreTRX PROPERTIES OUTPUT_NAME "MeshCoreTRX")
endif()
