cmake_minimum_required(VERSION 3.16)
project(visslink_tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(COMPONENT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/visslink)
add_executable(visslink_tests
  main.cpp test_decode.cpp test_scheduler.cpp test_kw_protocol.cpp test_simulation.cpp
  ${COMPONENT_DIR}/kw_protocol.cpp ${COMPONENT_DIR}/scheduler.cpp ${COMPONENT_DIR}/read_engine.cpp)
target_include_directories(visslink_tests PRIVATE ${COMPONENT_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(visslink_tests PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_options(visslink_tests PRIVATE -fsanitize=address,undefined)
enable_testing()
add_test(NAME visslink_tests COMMAND visslink_tests)
