#pragma once

// Minimal Arduino compatibility layer for building swd_min on macOS.
// Only implements what this project needs.

#include <cstddef>
#include <cstdint>

// Arduino constants
#define HIGH 0x1
#define LOW  0x0

#define INPUT          0x0
#define OUTPUT         0x1
#define INPUT_PULLUP   0x2
#define INPUT_PULLDOWN 0x3

// PROGMEM/Flash string helpers used by Arduino code
struct __FlashStringHelper;
#define F(str_literal) (reinterpret_cast<const __FlashStringHelper *>(str_literal))

// Forward declarations (implemented in sim/arduino_compat/arduino_compat.cpp)
void pinMode(int pin, int mode);
void digitalWrite(int pin, int value);
int  digitalRead(int pin);

void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);

// A few Arduino-ish types
using uint8_t  = std::uint8_t;
using uint16_t = std::uint16_t;
using uint32_t = std::uint32_t;
using int32_t  = std::int32_t;
using size_t   = std::size_t;

// Allow swd_min to compile without full Arduino headers
