#pragma once

#include <Arduino.h>

#include "firmware_source.h"

namespace firmware_source {

// A firmware_source::Reader wrapper that overrides the first 256 bytes with a
// caller-provided snapshot and passes through all remaining bytes.
//
// This is used for verifying: after a write that injects serial/unique_id, we
// keep a snapshot of the injected first block and verify against it.
class FirstBlockOverrideReader final : public Reader {
 public:
  FirstBlockOverrideReader(Reader &inner, const uint8_t *first_block, uint32_t first_block_len);

  uint32_t size() const override { return inner_.size(); }
  bool read_at(uint32_t offset, uint8_t *dst, uint32_t n, uint32_t *out_n) override;

 private:
  Reader &inner_;
  const uint8_t *first_block_;
  uint32_t first_block_len_;
};

}  // namespace firmware_source

