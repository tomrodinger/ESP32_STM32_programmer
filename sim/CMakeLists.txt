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
  ../src/stm32g0_prog.cpp
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

add_executable(reset_and_switch_to_swd_simulation
  reset_and_switch_to_swd_main.cpp
  arduino_compat/arduino_compat.cpp
  gpio_model.cpp
  logger.cpp
  stm32_swd_target.cpp
  ../src/swd_min.cpp
)

target_include_directories(reset_and_switch_to_swd_simulation PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/arduino_compat
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/..
  ${CMAKE_CURRENT_LIST_DIR}/../src
)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
  target_compile_options(reset_and_switch_to_swd_simulation PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(read_simulation
  read_simulation_main.cpp
  arduino_compat/arduino_compat.cpp
  gpio_model.cpp
  logger.cpp
  stm32_swd_target.cpp
  ../src/swd_min.cpp
)

target_include_directories(read_simulation PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/arduino_compat
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/..
  ${CMAKE_CURRENT_LIST_DIR}/../src
)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
  target_compile_options(read_simulation PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(write_simulation
  write_simulation_main.cpp
  arduino_compat/arduino_compat.cpp
  gpio_model.cpp
  logger.cpp
  stm32_swd_target.cpp
  ../src/swd_min.cpp
)

target_include_directories(write_simulation PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/arduino_compat
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/..
  ${CMAKE_CURRENT_LIST_DIR}/../src
)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
  target_compile_options(write_simulation PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(read_then_write_simulation
  read_then_write_simulation_main.cpp
  arduino_compat/arduino_compat.cpp
  gpio_model.cpp
  logger.cpp
  stm32_swd_target.cpp
  ../src/swd_min.cpp
)

target_include_directories(read_then_write_simulation PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/arduino_compat
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/..
  ${CMAKE_CURRENT_LIST_DIR}/../src
)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
  target_compile_options(read_then_write_simulation PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(write_then_read_simulation
  write_then_read_simulation_main.cpp
  arduino_compat/arduino_compat.cpp
  gpio_model.cpp
  logger.cpp
  stm32_swd_target.cpp
  ../src/swd_min.cpp
)

target_include_directories(write_then_read_simulation PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/arduino_compat
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/..
  ${CMAKE_CURRENT_LIST_DIR}/../src
)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
  target_compile_options(write_then_read_simulation PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(read_flash_simulation
  read_flash_simulation_main.cpp
  arduino_compat/arduino_compat.cpp
  gpio_model.cpp
  logger.cpp
  stm32_swd_target.cpp
  ../src/swd_min.cpp
  ../src/stm32g0_prog.cpp
)

target_include_directories(read_flash_simulation PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/arduino_compat
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/..
  ${CMAKE_CURRENT_LIST_DIR}/../src
)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
  target_compile_options(read_flash_simulation PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(erase_flash_simulation
  erase_flash_simulation_main.cpp
  arduino_compat/arduino_compat.cpp
  gpio_model.cpp
  logger.cpp
  stm32_swd_target.cpp
  ../src/swd_min.cpp
  ../src/stm32g0_prog.cpp
)

target_include_directories(erase_flash_simulation PRIVATE
  ${CMAKE_CURRENT_LIST_DIR}/arduino_compat
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_CURRENT_LIST_DIR}/..
  ${CMAKE_CURRENT_LIST_DIR}/../src
)

if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|AppleClang|GNU")
  target_compile_options(erase_flash_simulation PRIVATE -Wall -Wextra -Wpedantic)
endif()
