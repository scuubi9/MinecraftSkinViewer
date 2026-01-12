# ==============================
# File: CMakeLists.txt
# ==============================
cmake_minimum_required(VERSION 3.20)
project(MinecraftSkinViewer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)


# WIN32 => no console window. MinGW needs -municode for wWinMain.
add_executable(MinecraftSkinViewer WIN32
  src/main.cpp
)

# ---- Dear ImGui (external/imgui) ----
target_sources(MinecraftSkinViewer PRIVATE
  external/imgui/imgui.cpp
  external/imgui/imgui_draw.cpp
  external/imgui/imgui_tables.cpp
  external/imgui/imgui_widgets.cpp
  external/imgui/backends/imgui_impl_win32.cpp
  external/imgui/backends/imgui_impl_dx11.cpp
)

target_include_directories(MinecraftSkinViewer PRIVATE
  src
  external/imgui
  external/imgui/backends
  external/DirectXMath/Inc
)

target_compile_definitions(MinecraftSkinViewer PRIVATE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

# MinGW: use Unicode entry point (wWinMain) + wide-char argv/wargv plumbing.
if (MINGW)
  target_link_options(MinecraftSkinViewer PRIVATE -municode)
endif()

target_link_libraries(MinecraftSkinViewer PRIVATE
  d3d11
  dxgi
  d3dcompiler
  windowscodecs
  dwmapi
  shell32
  user32
  gdi32
  ole32
)
