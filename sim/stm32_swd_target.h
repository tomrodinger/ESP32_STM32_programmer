#pragma once

#include <cstdint>

namespace sim {

// Very small SWD target model sufficient for DP IDCODE read.
// It is driven by SWCLK rising edges and can choose to drive SWDIO.
class Stm32SwdTarget {
public:
  void reset();

  // Host-to-target observation on each SWCLK rising edge.
  // host_driving indicates whether host is actively driving SWDIO (OUTPUT mode).
  // host_level is the logic level on SWDIO as seen at that edge.
  void on_swclk_rising_edge(bool host_driving, uint8_t host_level);

  // Marker helper: true if the target sampled a host-driven bit on the last edge.
  // Consumed by the simulator shim for waveform overlay; cleared when read.
  bool consume_sampled_host_bit_flag();

  // Target drive control for SWDIO.
  bool drive_enabled() const { return drive_en_; }
  uint8_t drive_level() const { return drive_level_; }

  // Config
  void set_idcode(uint32_t idcode) { idcode_ = idcode; }

private:
  enum class Phase : uint8_t {
    AwaitResetOrSeq,
    CollectSeq,
    CollectRequest,
    TurnaroundToTarget,
    SendAck,
    SendData,
    SendParity,
    TurnaroundToHost,
  };

  // Helpers
  static uint8_t parity_u32(uint32_t v);

  // State
  Phase phase_ = Phase::AwaitResetOrSeq;

  // line reset detection
  uint32_t consecutive_high_cycles_ = 0;
  bool line_reset_seen_ = false;

  // JTAG-to-SWD sequence
  uint16_t seq_shift_ = 0;
  uint8_t seq_bits_ = 0;

  // Request collection
  uint8_t req_shift_ = 0;
  uint8_t req_bits_ = 0;
  bool swd_enabled_ = false;

  // Response
  uint32_t idcode_ = 0x2BA01477; // placeholder; can be overridden
  uint8_t bit_idx_ = 0;
  uint32_t data_shift_ = 0;
  uint8_t parity_bit_ = 0;

  bool drive_en_ = false;
  uint8_t drive_level_ = 1;

  bool sampled_host_bit_ = false;
};

} // namespace sim
