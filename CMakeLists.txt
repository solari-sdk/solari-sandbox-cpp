cmake_minimum_required(VERSION 3.16)
project(solari_sdk CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(SOLARI_WITH_WS "Build the IXWebSocket control transport" ON)
option(SOLARI_BUILD_TESTS "Build the offline test suite" ON)
option(SOLARI_BUILD_LIVE_TESTS "Build the live (staging) integration suite" OFF)

include(FetchContent)

# --- nlohmann/json ----------------------------------------------------------
FetchContent_Declare(
  nlohmann_json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
)
FetchContent_MakeAvailable(nlohmann_json)

# --- libcurl (system) + threads ---------------------------------------------
find_package(CURL REQUIRED)
find_package(Threads REQUIRED)

set(SOLARI_SOURCES
  src/util.cpp
  src/errors.cpp
  src/http.cpp
  src/control_channel.cpp
  src/code.cpp
  src/git.cpp
  src/sandbox.cpp
  src/client.cpp
)

# --- IXWebSocket (optional; TLS via OpenSSL) --------------------------------
if(SOLARI_WITH_WS)
  set(USE_TLS ON CACHE BOOL "" FORCE)
  set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
  set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG v11.4.5
  )
  FetchContent_MakeAvailable(ixwebsocket)
  list(APPEND SOLARI_SOURCES src/ws_transport.cpp)
endif()

add_library(solari ${SOLARI_SOURCES})
target_include_directories(solari PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(solari PUBLIC
  nlohmann_json::nlohmann_json
  CURL::libcurl
  Threads::Threads
)
if(SOLARI_WITH_WS)
  target_link_libraries(solari PUBLIC ixwebsocket)
  target_compile_definitions(solari PUBLIC SOLARI_WITH_WS=1)
endif()

# --- tests (doctest, offline) -----------------------------------------------
if(SOLARI_BUILD_TESTS)
  FetchContent_Declare(
    doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
  )
  FetchContent_MakeAvailable(doctest)

  add_executable(solari_tests tests/test_main.cpp tests/test_contract_wire.cpp)
  target_link_libraries(solari_tests PRIVATE solari doctest::doctest)
  # The cross-language wire contract the TS reference SDK regenerates. Passed as a
  # path because ctest's working directory is the build tree, not this one.
  target_compile_definitions(solari_tests PRIVATE
    SOLARI_CONTRACT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/ts-request-fixtures.json")

  enable_testing()
  add_test(NAME solari_tests COMMAND solari_tests)
endif()

# --- live tests (staging; opt-in, NOT registered with ctest) -----------------
# Built as a separate binary so `ctest` stays hermetic and offline. Needs the WS
# transport to open a control channel, and is additionally gated at runtime on
# SOLARI_LIVE so it cannot run by accident.
#
#   cmake -S . -B build -DSOLARI_BUILD_LIVE_TESTS=ON && cmake --build build
#   SOLARI_LIVE=1 SOLARI_API_KEY=… ./build/solari_live_tests -s
if(SOLARI_BUILD_LIVE_TESTS)
  if(NOT SOLARI_WITH_WS)
    message(FATAL_ERROR "SOLARI_BUILD_LIVE_TESTS=ON requires SOLARI_WITH_WS=ON")
  endif()
  if(NOT SOLARI_BUILD_TESTS)
    # doctest is fetched in the SOLARI_BUILD_TESTS block above; pull it in when
    # only the live suite is requested.
    FetchContent_Declare(
      doctest
      URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.11.tar.gz
    )
    FetchContent_MakeAvailable(doctest)
  endif()
  add_executable(solari_live_tests tests/test_live.cpp)
  target_link_libraries(solari_live_tests PRIVATE solari doctest::doctest)
endif()
