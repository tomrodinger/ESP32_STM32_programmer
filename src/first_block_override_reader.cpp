#include "first_block_override_reader.h"

#include <cstring>

namespace firmware_source {

static constexpr uint32_t k_first_block_size = 256u;

FirstBlockOverrideReader::FirstBlockOverrideReader(Reader &inner, const uint8_t *first_block, uint32_t first_block_len)
    : inner_(inner), first_block_(first_block), first_block_len_(first_block_len) {
  if (first_block_len_ > k_first_block_size) first_block_len_ = k_first_block_size;
}

bool FirstBlockOverrideReader::read_at(uint32_t offset, uint8_t *dst, uint32_t n, uint32_t *out_n) {
  if (out_n) *out_n = 0;
  if (!dst && n != 0) return false;
  if (n == 0) return true;

  const uint32_t sz = size();
  if (offset > sz) return false;

  // If we don't have an override snapshot, just pass through.
  if (!first_block_ || first_block_len_ == 0) {
    return inner_.read_at(offset, dst, n, out_n);
  }

  // Fully outside first block -> pass through.
  if (offset >= k_first_block_size) {
    return inner_.read_at(offset, dst, n, out_n);
  }

  // Split request if it crosses the 256-byte boundary.
  const uint32_t n_in_first = (offset + n <= k_first_block_size) ? n : (k_first_block_size - offset);
  const uint32_t n_after = n - n_in_first;

  // Serve first part from snapshot (pad beyond provided len with 0xFF).
  for (uint32_t i = 0; i < n_in_first; i++) {
    const uint32_t idx = offset + i;
    dst[i] = (idx < first_block_len_) ? first_block_[idx] : 0xFF;
  }

  if (n_after == 0) {
    if (out_n) *out_n = n;
    return true;
  }

  uint32_t got2 = 0;
  if (!inner_.read_at(k_first_block_size, dst + n_in_first, n_after, &got2)) {
    return false;
  }
  if (out_n) *out_n = n_in_first + got2;
  return true;
}

}  // namespace firmware_source

