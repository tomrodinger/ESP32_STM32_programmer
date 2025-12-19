#pragma once

#include <Arduino.h>

namespace stm32g0_prog {

// Target specifics (STM32G031)
static constexpr uint32_t FLASH_BASE = 0x08000000u;
static constexpr uint32_t FLASH_SIZE_BYTES = 0x10000u;     // 64KB
static constexpr uint32_t FLASH_PAGE_SIZE_BYTES = 2048u;   // 2KB

// Connect to target over SWD and halt the core.
bool connect_and_halt();

// Flash operations
bool flash_mass_erase();
bool flash_program(uint32_t addr, const uint8_t *data, uint32_t len);

// Verify + dump bytes read from flash. Returns true only if all bytes match.
bool flash_verify_and_dump(uint32_t addr, const uint8_t *data, uint32_t len);

} // namespace stm32g0_prog

