#include "stm32_swd_target.h"

namespace sim {

static inline uint8_t get_bit_u32(uint32_t v, uint8_t i) { return (v >> i) & 1u; }

uint8_t Stm32SwdTarget::parity_u32(uint32_t v) {
  uint8_t p = 0;
  while (v) {
    p ^= 1;
    v &= (v - 1);
  }
  return p;
}

void Stm32SwdTarget::reset() {
  phase_ = Phase::AwaitResetOrSeq;
  consecutive_high_cycles_ = 0;
  line_reset_seen_ = false;
  seq_shift_ = 0;
  seq_bits_ = 0;
  req_shift_ = 0;
  req_bits_ = 0;
  swd_enabled_ = false;
  bit_idx_ = 0;
  data_shift_ = 0;
  parity_bit_ = 0;
  drive_en_ = false;
  drive_level_ = 1;
}

void Stm32SwdTarget::on_swclk_rising_edge(bool host_driving, uint8_t host_level) {
  // Detect line reset: consecutive cycles where host drives SWDIO high.
  // Important: line reset is used both *before* and *after* SWD is enabled.
  //  - Before SWD enabled: line reset is typically followed by 0xE79E.
  //  - After SWD enabled: line reset just resets the SWD transaction layer and is NOT followed by 0xE79E.

  if (host_driving && host_level == 1) {
    consecutive_high_cycles_++;
  } else {
    consecutive_high_cycles_ = 0;
  }

  if (consecutive_high_cycles_ >= 50) {
    line_reset_seen_ = true;
    drive_en_ = false;

    if (swd_enabled_) {
      // In SWD mode: after a line reset, simply hunt for a request.
      phase_ = Phase::CollectRequest;
      req_shift_ = 0;
      req_bits_ = 0;
    } else {
      // Not in SWD yet: after a line reset, wait for the first non-high bit,
      // then start capturing the 16-bit 0xE79E sequence.
      phase_ = Phase::AwaitResetOrSeq;
      seq_shift_ = 0;
      seq_bits_ = 0;
    }
  }

  switch (phase_) {
    case Phase::AwaitResetOrSeq: {
      // Pre-SWD: only start capturing 0xE79E after we've seen a line reset and
      // then the host begins sending something other than continuous '1's.
      if (!swd_enabled_ && line_reset_seen_ && host_driving && host_level == 0) {
        phase_ = Phase::CollectSeq;
        seq_shift_ = 0;
        seq_bits_ = 0;
        // fallthrough: capture this first 0 bit as bit0 of sequence
      } else {
        return;
      }
    }

    case Phase::CollectSeq: {
      if (!host_driving) {
        // Ignore if host released (shouldn't happen during sequence)
        return;
      }
      // Shift in LSB-first bits of the 16-bit sequence.
      seq_shift_ |= (uint16_t)(host_level & 1u) << seq_bits_;
      seq_bits_++;
      if (seq_bits_ >= 16) {
        if (seq_shift_ == 0xE79E) {
          swd_enabled_ = true;
          phase_ = Phase::CollectRequest;
          req_shift_ = 0;
          req_bits_ = 0;
        } else {
          // Wrong sequence; keep hunting after the next line reset.
          swd_enabled_ = false;
          phase_ = Phase::AwaitResetOrSeq;
        }
        // Once we've tried a sequence, require another line reset before trying again.
        line_reset_seen_ = false;
      }
      return;
    }

    case Phase::CollectRequest: {
      // We only collect request when host is driving.
      if (!host_driving) {
        // This is the turnaround cycle; after request, host releases for 1 cycle.
        // We expect this once we've collected 8 bits.
        return;
      }

      // Collect 8-bit request header (LSB-first)
      req_shift_ |= (uint8_t)(host_level & 1u) << req_bits_;
      req_bits_++;

      if (req_bits_ >= 8) {
        // Parse request
        const uint8_t start = (req_shift_ >> 0) & 1u;
        const uint8_t apndp = (req_shift_ >> 1) & 1u;
        const uint8_t rnw   = (req_shift_ >> 2) & 1u;
        const uint8_t a2    = (req_shift_ >> 3) & 1u;
        const uint8_t a3    = (req_shift_ >> 4) & 1u;
        const uint8_t par   = (req_shift_ >> 5) & 1u;
        const uint8_t stop  = (req_shift_ >> 6) & 1u;
        const uint8_t park  = (req_shift_ >> 7) & 1u;

        const uint8_t parity_calc = (uint8_t)(apndp ^ rnw ^ a2 ^ a3);
        const uint8_t addr = (uint8_t)((a3 << 3) | (a2 << 2));

        const bool header_ok = (start == 1u) && (stop == 0u) && (park == 1u) && (parity_calc == par);

        // Prepare response only for DP read IDCODE (addr 0x00)
        const bool is_dp = (apndp == 0u);
        const bool is_read = (rnw == 1u);
        const bool is_idcode = (addr == 0x00);

        if (swd_enabled_ && header_ok && is_dp && is_read && is_idcode) {
          // Next SWCLK rising edge after the turnaround cycle we begin driving ACK.
          phase_ = Phase::TurnaroundToTarget;
          bit_idx_ = 0;
          data_shift_ = idcode_;
          parity_bit_ = parity_u32(idcode_);
        } else {
          // Ignore any other requests for now.
          phase_ = Phase::CollectRequest;
        }

        // Reset request collector for the next transaction.
        req_shift_ = 0;
        req_bits_ = 0;
      }
      return;
    }

    case Phase::TurnaroundToTarget:
      // This rising edge corresponds to the turnaround cycle (host released).
      // We start driving ACK on the *next* edge.
      drive_en_ = true;
      phase_ = Phase::SendAck;
      bit_idx_ = 0;
      return;

    case Phase::SendAck: {
      // ACK is 3 bits, LSB-first. OK = 0b001.
      const uint8_t ack_ok = 0b001;
      drive_level_ = (ack_ok >> bit_idx_) & 1u;
      bit_idx_++;
      if (bit_idx_ >= 3) {
        phase_ = Phase::SendData;
        bit_idx_ = 0;
      }
      return;
    }

    case Phase::SendData: {
      drive_level_ = get_bit_u32(data_shift_, bit_idx_);
      bit_idx_++;
      if (bit_idx_ >= 32) {
        phase_ = Phase::SendParity;
      }
      return;
    }

    case Phase::SendParity:
      drive_level_ = parity_bit_;
      phase_ = Phase::TurnaroundToHost;
      return;

    case Phase::TurnaroundToHost:
      // After parity, the protocol has a turnaround where target releases.
      drive_en_ = false;
      phase_ = Phase::CollectRequest;
      return;

    default:
      return;
  }
}

} // namespace sim
