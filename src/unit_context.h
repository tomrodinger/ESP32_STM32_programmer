#pragma once

#include <Arduino.h>

namespace unit_context {

// Holds the identity for the most recently programmed unit.
// Part 2 (RS485 upgrade) and Part 3 (tests) need the unique_id that was
// programmed during Part 1.
struct Context {
  bool valid = false;
  uint32_t serial = 0;
  uint64_t unique_id = 0;
};

void set(const Context &c);
Context get();
void clear();

}  // namespace unit_context

