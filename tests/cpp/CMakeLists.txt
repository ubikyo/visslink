cmake_minimum_required(VERSION 3.16)
project(visslink_tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Le cœur du composant (kw_protocol.h, scheduler.h, decode.h, schedule.h) est
# en en-têtes seuls.
add_executable(visslink_tests
  main.cpp test_decode.cpp test_scheduler.cpp test_kw_protocol.cpp test_simulation.cpp
  test_schedule.cpp test_kw_write.cpp test_write_guard.cpp test_boost.cpp test_dhw.cpp test_heating.cpp
  test_schedule_editor.cpp test_burner.cpp)
target_include_directories(visslink_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../components/visslink ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(visslink_tests PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
target_link_options(visslink_tests PRIVATE -fsanitize=address,undefined)
