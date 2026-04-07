cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" OFF)
option(ENABLE_QT "Use Qt functionality" OFF)

# Disable -Werror for third-party libraries (Xcode applies project-wide)
set(CMAKE_COMPILE_WARNING_AS_ERROR OFF)
include(compilerconfig)
include(defaults)
include(helpers)

add_library(${CMAKE_PROJECT_NAME} MODULE)

# ─── External libraries: WebSocket + JSON ───
include(FetchContent)

# macOS: Homebrew OpenSSL path (skip if preset already set)
if(APPLE AND NOT DEFINED CACHE{OPENSSL_ROOT_DIR})
  execute_process(
    COMMAND brew --prefix openssl
    OUTPUT_VARIABLE OPENSSL_ROOT_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
  )
  set(OPENSSL_ROOT_DIR "${OPENSSL_ROOT_DIR}" CACHE PATH "" FORCE)
endif()

# IXWebSocket — C++ WebSocket client (Deepgram connection)
# Use OpenSSL (macOS SecureTransport is deprecated)
FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG        v11.4.5
  GIT_SHALLOW    TRUE
)
set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
set(USE_SECURE_TRANSPORT OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ixwebsocket)

# nlohmann/json — JSON parsing (header-only)
FetchContent_Declare(
  json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
  GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(json)

find_package(libobs REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs ixwebsocket nlohmann_json::nlohmann_json)

if(ENABLE_FRONTEND_API)
  find_package(obs-frontend-api REQUIRED)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)
endif()

if(ENABLE_QT)
  find_package(Qt6 COMPONENTS Widgets Core)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
  target_compile_options(
    ${CMAKE_PROJECT_NAME}
    PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
  )
  set_target_properties(
    ${CMAKE_PROJECT_NAME}
    PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
  )
endif()

target_sources(${CMAKE_PROJECT_NAME} PRIVATE src/plugin-main.cpp)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
