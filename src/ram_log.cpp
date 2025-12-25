#include "ram_log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace ram_log {

// Keep it modest to avoid starving WiFi/HTTP stacks.
//
// NOTE: This is a static buffer in .bss. If you change it, keep an eye on RAM
// usage and stability under WiFi load.
#ifndef RAM_LOG_CAPACITY_BYTES
#define RAM_LOG_CAPACITY_BYTES (64u * 1024u)
#endif

static constexpr size_t k_capacity_bytes = (size_t)RAM_LOG_CAPACITY_BYTES;

static uint8_t g_buf[k_capacity_bytes];
static size_t g_head = 0;   // next write index [0..cap)
static size_t g_count = 0;  // bytes currently stored [0..cap]
static uint64_t g_total_written = 0;

static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static bool g_inited = false;

void begin() {
  portENTER_CRITICAL(&g_mux);
  if (!g_inited) {
    g_head = 0;
    g_count = 0;
    g_total_written = 0;
    g_inited = true;
  }
  portEXIT_CRITICAL(&g_mux);
}

size_t capacity() { return k_capacity_bytes; }

void clear() {
  if (!g_inited) begin();
  portENTER_CRITICAL(&g_mux);
  g_head = 0;
  g_count = 0;
  portEXIT_CRITICAL(&g_mux);
}

size_t size() {
  if (!g_inited) begin();
  portENTER_CRITICAL(&g_mux);
  const size_t n = g_count;
  portEXIT_CRITICAL(&g_mux);
  return n;
}

uint64_t total_written() {
  if (!g_inited) begin();
  portENTER_CRITICAL(&g_mux);
  const uint64_t n = g_total_written;
  portEXIT_CRITICAL(&g_mux);
  return n;
}

void write(const uint8_t *data, size_t len) {
  if (!data || len == 0) return;
  if (!g_inited) begin();

  portENTER_CRITICAL(&g_mux);
  for (size_t i = 0; i < len; i++) {
    g_buf[g_head] = data[i];
    g_head++;
    if (g_head >= k_capacity_bytes) g_head = 0;
    if (g_count < k_capacity_bytes) {
      g_count++;
    }
    g_total_written++;
  }
  portEXIT_CRITICAL(&g_mux);
}

static void snapshot_meta(size_t *out_start, size_t *out_count) {
  if (!out_start || !out_count) return;
  portENTER_CRITICAL(&g_mux);
  const size_t head = g_head;
  const size_t count = g_count;
  portEXIT_CRITICAL(&g_mux);

  *out_start = (head + k_capacity_bytes - count) % k_capacity_bytes;
  *out_count = count;
}

void snapshot_spans(const uint8_t **span1_ptr, size_t *span1_len, const uint8_t **span2_ptr, size_t *span2_len) {
  if (span1_ptr) *span1_ptr = nullptr;
  if (span1_len) *span1_len = 0;
  if (span2_ptr) *span2_ptr = nullptr;
  if (span2_len) *span2_len = 0;

  if (!g_inited) begin();

  size_t start = 0;
  size_t count = 0;
  snapshot_meta(&start, &count);

  if (count == 0) return;

  const size_t first = k_capacity_bytes - start;
  const size_t n1 = (count < first) ? count : first;
  const size_t n2 = count - n1;

  if (span1_ptr) *span1_ptr = &g_buf[start];
  if (span1_len) *span1_len = n1;

  if (n2 > 0) {
    if (span2_ptr) *span2_ptr = &g_buf[0];
    if (span2_len) *span2_len = n2;
  }
}

size_t snapshot(uint8_t *out, size_t out_cap) {
  if (!out || out_cap == 0) return 0;
  if (!g_inited) begin();

  size_t start = 0;
  size_t count = 0;
  snapshot_meta(&start, &count);

  const size_t to_copy = (count < out_cap) ? count : out_cap;
  for (size_t i = 0; i < to_copy; i++) {
    out[i] = g_buf[(start + i) % k_capacity_bytes];
  }
  return to_copy;
}

static void stream_from_meta(Print &out, size_t start, size_t count) {
  // Larger chunk reduces per-call overhead when streaming over WiFi.
  uint8_t chunk[1024];
  size_t done = 0;
  while (done < count) {
    const size_t want = (count - done > sizeof(chunk)) ? sizeof(chunk) : (count - done);
    for (size_t i = 0; i < want; i++) {
      chunk[i] = g_buf[(start + done + i) % k_capacity_bytes];
    }
    out.write(chunk, want);
    done += want;
  }
}

void stream_to(Print &out) {
  if (!g_inited) begin();

  size_t start = 0;
  size_t count = 0;
  snapshot_meta(&start, &count);
  stream_from_meta(out, start, count);
}

void stream_to_n(Print &out, size_t n) {
  if (!g_inited) begin();

  size_t start = 0;
  size_t count = 0;
  snapshot_meta(&start, &count);
  const size_t to_stream = (n < count) ? n : count;
  stream_from_meta(out, start, to_stream);
}

}  // namespace ram_log
