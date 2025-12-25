#pragma once

#include <Arduino.h>

// Byte-based circular RAM log.
//
// Goal: keep a rolling window of recent output so it can be viewed/downloaded
// via the WiFi web UI and dumped via the serial console.
//
// Design:
// - fixed-size circular buffer (no malloc)
// - byte-based (not line-based)
// - thread-safe for multi-core/task writers
namespace ram_log {

// Initialize the RAM log. Safe to call multiple times.
void begin();

// Append bytes to the circular buffer.
void write(const uint8_t *data, size_t len);

// Reset the buffer to empty.
void clear();

// Maximum bytes retained in RAM.
size_t capacity();

// Current bytes stored (<= capacity).
size_t size();

// Total bytes written since boot (monotonic; wraps at uint64).
uint64_t total_written();

// Copy the current buffer contents (oldest -> newest) into out.
// Returns number of bytes copied.
size_t snapshot(uint8_t *out, size_t out_cap);

// Stream the current buffer contents (oldest -> newest) to a Print sink.
// Uses a small stack chunk; does not allocate.
void stream_to(Print &out);

// Stream up to `n` bytes from the current snapshot (oldest -> newest).
// If `n` exceeds the current size(), it streams only size() bytes.
void stream_to_n(Print &out, size_t n);

// Return up to two linear spans (because the circular buffer may wrap) for a
// stable snapshot.
//
// - Span order is oldest -> newest.
// - When the snapshot does not wrap, span2 will be empty.
// - This is intended for efficient bulk streaming (e.g. HTTP download) without
//   copying.
void snapshot_spans(const uint8_t **span1_ptr, size_t *span1_len, const uint8_t **span2_ptr, size_t *span2_len);

}  // namespace ram_log
