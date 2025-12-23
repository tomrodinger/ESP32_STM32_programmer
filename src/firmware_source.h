#pragma once

#include <Arduino.h>

namespace firmware_source {

// Minimal read-at-offset interface.
// Implementation must return true and set out_n to the number of bytes read.
// When offset == size, out_n must be 0.
class Reader {
 public:
  virtual ~Reader() = default;
  virtual uint32_t size() const = 0;
  virtual bool read_at(uint32_t offset, uint8_t *dst, uint32_t n, uint32_t *out_n) = 0;
};

}  // namespace firmware_source

// Bridge helper: stm32g0_prog defines its own FirmwareReader to avoid pulling in
// higher-level headers. This adapter lets us pass a firmware_source::Reader into
// stm32g0_prog APIs.
#include "stm32g0_prog.h"

namespace firmware_source {

class Stm32G0Adapter final : public stm32g0_prog::FirmwareReader {
 public:
  explicit Stm32G0Adapter(Reader &r) : r_(r) {}
  uint32_t size() const override { return r_.size(); }
  bool read_at(uint32_t offset, uint8_t *dst, uint32_t n, uint32_t *out_n) override {
    return r_.read_at(offset, dst, n, out_n);
  }

 private:
  Reader &r_;
};

}  // namespace firmware_source
