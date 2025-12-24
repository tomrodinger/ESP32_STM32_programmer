#include "product_info_injector_reader.h"

#include <cstring>

#include "product_info.h"  // authoritative layout + address

#include "stm32g0_prog.h"  // FLASH_BASE

namespace firmware_source {

static constexpr uint32_t k_first_block_size = 256u;

ProductInfoInjectorReader::ProductInfoInjectorReader(Reader &inner, uint32_t serial, uint64_t unique_id)
    : inner_(inner), serial_(serial), unique_id_(unique_id) {
  memset(first_block_, 0xFF, sizeof(first_block_));
}

bool ProductInfoInjectorReader::ensure_first_block_loaded_and_patched() {
  if (first_loaded_) return true;

  uint32_t got = 0;
  if (!inner_.read_at(/*offset=*/0, first_block_, k_first_block_size, &got)) {
    return false;
  }
  // Pad beyond EOF with 0xFF (matches programming padding behavior).
  if (got < k_first_block_size) {
    memset(first_block_ + got, 0xFF, k_first_block_size - got);
  }

  // Patch only if the product_info struct fully fits inside the first block.
  static constexpr uint32_t k_pi_off = (uint32_t)(PRODUCT_INFO_MEMORY_LOCATION - stm32g0_prog::FLASH_BASE);
  static_assert(PRODUCT_INFO_MEMORY_LOCATION == 0x08000010u || PRODUCT_INFO_MEMORY_LOCATION == 0x8000010u,
                "PRODUCT_INFO_MEMORY_LOCATION expected to be 0x08000010");
  static_assert(k_pi_off < k_first_block_size, "product_info offset must be inside first 256 bytes");
  static_assert(k_pi_off + sizeof(product_info_struct) <= k_first_block_size,
                "product_info struct must fit inside first 256 bytes");

  // Read existing struct bytes then update only requested fields.
  product_info_struct pi;
  memcpy(&pi, first_block_ + k_pi_off, sizeof(pi));
  pi.serial_number = serial_;
  pi.unique_id = unique_id_;
  memcpy(first_block_ + k_pi_off, &pi, sizeof(pi));

  first_loaded_ = true;
  return true;
}

bool ProductInfoInjectorReader::read_at(uint32_t offset, uint8_t *dst, uint32_t n, uint32_t *out_n) {
  if (out_n) *out_n = 0;
  if (!dst && n != 0) return false;

  const uint32_t sz = size();
  if (offset > sz) return false;
  if (n == 0) return true;

  // If request intersects first block, serve from cached/patched buffer.
  if (offset < k_first_block_size) {
    if (!ensure_first_block_loaded_and_patched()) return false;

    const uint32_t max_in_first = k_first_block_size - offset;
    const uint32_t take = (n < max_in_first) ? n : max_in_first;
    memcpy(dst, first_block_ + offset, take);
    if (out_n) *out_n = take;
    return true;
  }

  // Pass-through for all remaining data.
  return inner_.read_at(offset, dst, n, out_n);
}

}  // namespace firmware_source

