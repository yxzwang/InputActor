cmake_minimum_required(VERSION 3.20)
project(InputActorCpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(InputActorCpp WIN32
  src/main.cpp
)

target_compile_definitions(InputActorCpp PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

target_link_libraries(InputActorCpp PRIVATE
  nlohmann_json::nlohmann_json
  comctl32
  comdlg32
  shell32
  ole32
  user32
  gdi32
  advapi32
)

