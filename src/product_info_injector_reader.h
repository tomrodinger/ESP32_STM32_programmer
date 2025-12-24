#pragma once

#include <Arduino.h>

#include "firmware_source.h"

// A `firmware_source::Reader` wrapper that injects product-info fields
// (serial_number + unique_id) into the first 256-byte block.
//
// Design constraints from WIFI_CONFIG.md:
// - fixed 256-byte block
// - only first block is ever modified
// - apply modification once after the full first block is read
// - remaining bytes are pass-through
namespace firmware_source {

class ProductInfoInjectorReader final : public Reader {
 public:
  ProductInfoInjectorReader(Reader &inner, uint32_t serial, uint64_t unique_id);

  // Access to the patched first block for debug printing.
  // Valid after first read_at() that touches offset < 256.
  const uint8_t *first_block_ptr() const { return first_loaded_ ? first_block_ : nullptr; }
  static constexpr uint32_t first_block_size() { return 256u; }

  uint32_t size() const override { return inner_.size(); }
  bool read_at(uint32_t offset, uint8_t *dst, uint32_t n, uint32_t *out_n) override;

 private:
  bool ensure_first_block_loaded_and_patched();

  Reader &inner_;
  const uint32_t serial_;
  const uint64_t unique_id_;

  bool first_loaded_ = false;
  uint8_t first_block_[256];
};

}  // namespace firmware_source
