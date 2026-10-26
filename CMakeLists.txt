cmake_minimum_required(VERSION 3.20)
project(FFCSync LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# -DFFCSYNC_SANITIZE=thread (or address) builds everything with a sanitizer.
set(FFCSYNC_SANITIZE "" CACHE STRING "Sanitizer to enable: thread, address or empty")

find_package(Threads REQUIRED)

add_compile_options(-Wall -Wextra -Wpedantic)
if(FFCSYNC_SANITIZE)
  add_compile_options(-fsanitize=${FFCSYNC_SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${FFCSYNC_SANITIZE})
endif()

# ---- core library -----------------------------------------------------------
add_library(ffcsync_core STATIC
  src/model.cpp
  src/dataset.cpp
  src/rules.cpp
  src/registration_engine.cpp
  src/invariants.cpp
  src/result_io.cpp
  src/ipc_protocol.cpp
  src/concurrent_engine.cpp
  src/server.cpp
)
target_include_directories(ffcsync_core PUBLIC include)
target_link_libraries(ffcsync_core PUBLIC Threads::Threads)

# ---- executables ------------------------------------------------------------
add_executable(ffcsync_seq apps/seq_main.cpp)
target_link_libraries(ffcsync_seq PRIVATE ffcsync_core)
add_executable(ffcsync_server apps/server_main.cpp)
target_link_libraries(ffcsync_server PRIVATE ffcsync_core)
add_executable(ffcsync_client apps/client_main.cpp)
target_link_libraries(ffcsync_client PRIVATE ffcsync_core)

# ---- tests ------------------------------------------------------------------
enable_testing()
add_executable(ffcsync_tests
  tests/test_main.cpp
  tests/test_rules.cpp
  tests/test_invariants.cpp
  tests/test_golden.cpp
  tests/test_ipc.cpp
  tests/test_queue.cpp
  tests/test_concurrency.cpp
)
target_include_directories(ffcsync_tests PRIVATE third_party/doctest tests)
target_link_libraries(ffcsync_tests PRIVATE ffcsync_core)
target_compile_definitions(ffcsync_tests PRIVATE FFCSYNC_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
add_test(NAME unit_and_concurrency_tests COMMAND ffcsync_tests)
add_test(NAME integration_two_process
         COMMAND ${CMAKE_SOURCE_DIR}/tests/integration.sh ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR})
add_test(NAME integration_two_process_committer
         COMMAND ${CMAKE_SOURCE_DIR}/tests/integration.sh ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR} committer)
