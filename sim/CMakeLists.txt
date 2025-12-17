cmake_minimum_required(VERSION 3.20)
project(swd_sim LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(swd_sim
  main.cpp
  arduino_compat/arduino_compat.cpp
  gpio_model.cpp
  logger.cpp
  stm32_swd_target.cpp
  ../src/swd_min.cpp
)

target_include_directories(swd_sim PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/arduino_compat
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/..
  ${CMAKE_CURRENT_LIST_DIR}/../src
)

# Keep warnings reasonable for quick iteration
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
  target_compile_options(swd_sim PRIVATE -Wall -Wextra -Wpedantic)
endif()
