cmake_minimum_required(VERSION 3.20)
project(InputActorCpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(VIGEMCLIENT_DLL_PATH "" CACHE FILEPATH "Path to ViGEmClient.dll")
set(VIGEMBUS_SETUP_PATH "" CACHE FILEPATH "Path to ViGEmBusSetup.exe")

if(NOT EXISTS "${VIGEMCLIENT_DLL_PATH}")
  message(FATAL_ERROR "VIGEMCLIENT_DLL_PATH does not exist: ${VIGEMCLIENT_DLL_PATH}")
endif()
if(NOT EXISTS "${VIGEMBUS_SETUP_PATH}")
  message(FATAL_ERROR "VIGEMBUS_SETUP_PATH does not exist: ${VIGEMBUS_SETUP_PATH}")
endif()

file(TO_CMAKE_PATH "${VIGEMCLIENT_DLL_PATH}" VIGEMCLIENT_DLL_PATH_RC)
file(TO_CMAKE_PATH "${VIGEMBUS_SETUP_PATH}" VIGEMBUS_SETUP_PATH_RC)

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/src/resources.rc.in
  ${CMAKE_CURRENT_BINARY_DIR}/generated/resources.rc
  @ONLY
)

include(FetchContent)

FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

add_executable(InputActorCpp WIN32
  src/main.cpp
  src/resources.h
  ${CMAKE_CURRENT_BINARY_DIR}/generated/resources.rc
)

target_compile_definitions(InputActorCpp PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

target_include_directories(InputActorCpp PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}/generated
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
