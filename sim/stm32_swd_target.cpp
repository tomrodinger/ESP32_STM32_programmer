#include "stm32_swd_target.h"

#include <algorithm>
#include <cstring>

namespace sim {

// Address constants (subset)
static constexpr uint32_t FLASH_BASE = 0x08000000u;
static constexpr uint32_t FLASH_SIZE_BYTES = 0x10000u;

static constexpr uint32_t FLASH_REG_BASE = 0x40022000u;
static constexpr uint32_t FLASH_KEYR = FLASH_REG_BASE + 0x08u;
static constexpr uint32_t FLASH_SR = FLASH_REG_BASE + 0x10u;
static constexpr uint32_t FLASH_CR = FLASH_REG_BASE + 0x14u;

static constexpr uint32_t FLASH_KEY1 = 0x45670123u;
static constexpr uint32_t FLASH_KEY2 = 0xCDEF89ABu;

static constexpr uint32_t FLASH_SR_BSY = (1u << 16);

static constexpr uint32_t FLASH_CR_PG = (1u << 0);
static constexpr uint32_t FLASH_CR_MER1 = (1u << 2);
static constexpr uint32_t FLASH_CR_STRT = (1u << 16);
static constexpr uint32_t FLASH_CR_LOCK = (1u << 31);

// DP registers (addr bits [3:2] in request select these byte addresses)
static constexpr uint8_t DP_ADDR_IDCODE = 0x00;
static constexpr uint8_t DP_ADDR_ABORT = 0x00;
static constexpr uint8_t DP_ADDR_CTRLSTAT = 0x04;
static constexpr uint8_t DP_ADDR_SELECT = 0x08;
static constexpr uint8_t DP_ADDR_RDBUFF = 0x0C;

// AP registers (bank 0)
static constexpr uint8_t AP_ADDR_CSW = 0x00;
static constexpr uint8_t AP_ADDR_TAR = 0x04;
static constexpr uint8_t AP_ADDR_DRW = 0x0C;
static constexpr uint8_t AP_ADDR_IDR = 0xFC;

  uint8_t Stm32SwdTarget::parity_u32(uint32_t v) {
  uint8_t p = 0;
  while (v) {
    p ^= 1;
    v &= (v - 1);
  }
  return p;
}

void Stm32SwdTarget::flash_reset() {
  flash_.assign(FLASH_SIZE_BYTES, 0xFF);
  flash_keyr_last_ = 0;
  flash_sr_ = 0;
  flash_cr_ = FLASH_CR_LOCK;
  flash_bsy_clear_time_ns_ = 0;
}

void Stm32SwdTarget::flash_start_busy(uint64_t duration_ns) {
  flash_sr_ |= FLASH_SR_BSY;
  flash_bsy_clear_time_ns_ = t_ns_ + duration_ns;
}

void Stm32SwdTarget::flash_update_busy() {
  if ((flash_sr_ & FLASH_SR_BSY) && t_ns_ >= flash_bsy_clear_time_ns_) {
    flash_sr_ &= ~FLASH_SR_BSY;
    flash_bsy_clear_time_ns_ = 0;

    // If an erase completed, clear MER1/STRT bits (hardware typically clears STRT)
    flash_cr_ &= ~(FLASH_CR_MER1 | FLASH_CR_STRT);
  }
}

void Stm32SwdTarget::flash_try_unlock(uint32_t key) {
  if (!(flash_cr_ & FLASH_CR_LOCK)) return;

  if (flash_keyr_last_ == 0 && key == FLASH_KEY1) {
    flash_keyr_last_ = FLASH_KEY1;
    return;
  }

  if (flash_keyr_last_ == FLASH_KEY1 && key == FLASH_KEY2) {
    flash_cr_ &= ~FLASH_CR_LOCK;
    flash_keyr_last_ = 0;
    return;
  }

  // Wrong sequence resets.
  flash_keyr_last_ = 0;
}

void Stm32SwdTarget::flash_start_mass_erase() {
  // Only if unlocked.
  if (flash_cr_ & FLASH_CR_LOCK) return;

  // Erase simulated by setting all bytes to 0xFF.
  std::fill(flash_.begin(), flash_.end(), 0xFF);

  // Busy for a while to exercise wait loops.
  flash_start_busy(50ull * 1000ull * 1000ull); // 50ms
}

void Stm32SwdTarget::flash_program32(uint32_t addr, uint32_t v) {
  if (flash_cr_ & FLASH_CR_LOCK) return;
  if (!(flash_cr_ & FLASH_CR_PG)) return;

  // Program can only change 1->0 in real flash; simulate by AND.
  if (addr < FLASH_BASE || addr + 4 > FLASH_BASE + FLASH_SIZE_BYTES) return;
  const uint32_t off = addr - FLASH_BASE;
  for (uint32_t i = 0; i < 4; i++) {
    const uint8_t b = (uint8_t)((v >> (8 * i)) & 0xFF);
    flash_[off + i] = (uint8_t)(flash_[off + i] & b);
  }

  // Short busy to exercise polling.
  flash_start_busy(200ull * 1000ull); // 200us
}

bool Stm32SwdTarget::mem_read32(uint32_t addr, uint32_t &out) {
  flash_update_busy();

  // Flash array
  if (addr >= FLASH_BASE && addr + 4 <= FLASH_BASE + FLASH_SIZE_BYTES) {
    const uint32_t off = addr - FLASH_BASE;
    out = (uint32_t)flash_[off + 0] |
          ((uint32_t)flash_[off + 1] << 8) |
          ((uint32_t)flash_[off + 2] << 16) |
          ((uint32_t)flash_[off + 3] << 24);
    return true;
  }

  // Flash regs
  if (addr == FLASH_SR) {
    out = flash_sr_;
    return true;
  }
  if (addr == FLASH_CR) {
    out = flash_cr_;
    return true;
  }

  // DHCSR (just claim halted; this keeps connect_and_halt happy)
  if (addr == 0xE000EDF0u) {
    out = (1u << 17);
    return true;
  }

  // Default unmapped returns 0.
  out = 0;
  return true;
}

bool Stm32SwdTarget::mem_write32(uint32_t addr, uint32_t v) {
  flash_update_busy();

  // Flash regs
  if (addr == FLASH_KEYR) {
    flash_try_unlock(v);
    return true;
  }

  if (addr == FLASH_CR) {
    flash_cr_ = v;

    // If MER1|STRT is set, start mass erase.
    if ((flash_cr_ & FLASH_CR_MER1) && (flash_cr_ & FLASH_CR_STRT)) {
      flash_start_mass_erase();
    }
    return true;
  }

  // Flash programming: if within flash and PG set, treat as program.
  if (addr >= FLASH_BASE && addr + 4 <= FLASH_BASE + FLASH_SIZE_BYTES) {
    flash_program32(addr, v);
    return true;
  }

  // DHCSR write: ignore.
  if (addr == 0xE000EDF0u) return true;

  // Default: ignore.
  return true;
}

uint32_t Stm32SwdTarget::dp_read_reg(uint8_t addr) {
  switch (addr) {
    case DP_ADDR_IDCODE:
      return dp_idcode_;
    case DP_ADDR_CTRLSTAT:
      // Mirror power-up ack bits if requested.
      // host sets bits 30 and 28; we respond with 31 and 29.
      {
        uint32_t v = dp_ctrlstat_;
        const bool sys_req = (v >> 30) & 1u;
        const bool dbg_req = (v >> 28) & 1u;
        if (sys_req) v |= (1u << 31);
        if (dbg_req) v |= (1u << 29);
        dp_ctrlstat_ = v;
        return v;
      }
    case DP_ADDR_SELECT:
      return dp_select_;
    case DP_ADDR_RDBUFF:
      return dp_rdbuff_;
    default:
      return 0;
  }
}

void Stm32SwdTarget::dp_write_reg(uint8_t addr, uint32_t v) {
  switch (addr) {
    case DP_ADDR_ABORT:
      // Clear sticky errors etc (ignored)
      (void)v;
      return;
    case DP_ADDR_CTRLSTAT:
      dp_ctrlstat_ = v;
      return;
    case DP_ADDR_SELECT:
      dp_select_ = v;
      return;
    default:
      return;
  }
}

uint32_t Stm32SwdTarget::ap_read_reg(uint8_t addr) {
  // Only AHB-AP bank 0.
  if (addr == AP_ADDR_CSW) return ap_csw_;
  if (addr == AP_ADDR_TAR) return ap_tar_;

  if (addr == AP_ADDR_IDR) {
    // Dummy AHB-AP IDR value.
    return 0x24770011u;
  }

  if (addr == AP_ADDR_DRW) {
    uint32_t v = 0;
    (void)mem_read32(ap_tar_, v);

    // Auto-increment for SIZE=32 + AddrInc=single (this matches swd_min usage)
    ap_tar_ += 4;
    return v;
  }

  return 0;
}

void Stm32SwdTarget::ap_write_reg(uint8_t addr, uint32_t v) {
  if (addr == AP_ADDR_CSW) {
    ap_csw_ = v;
    return;
  }
  if (addr == AP_ADDR_TAR) {
    ap_tar_ = v;
    return;
  }
  if (addr == AP_ADDR_DRW) {
    (void)mem_write32(ap_tar_, v);
    ap_tar_ += 4;
    return;
  }
}

void Stm32SwdTarget::reset() {
  t_ns_ = 0;
  phase_ = Phase::AwaitResetOrSeq;
  consecutive_high_cycles_ = 0;
  line_reset_seen_ = false;
  post_reset_high_cycles_ = 0;
  seq_shift_ = 0;
  seq_bits_ = 0;
  req_shift_ = 0;
  req_bits_ = 0;
  swd_enabled_ = false;
  after_jtag_to_swd_ = false;
  req_kind_ = ReqKind::None;
  req_addr_ = 0;
  read_data_ = 0;
  read_parity_ = 0;
  bit_idx_ = 0;
  write_data_ = 0;
  write_bit_idx_ = 0;
  write_parity_rx_ = 0;
  drive_en_ = false;
  drive_level_ = 1;
  sampled_host_bit_ = false;

  last_target_sample_bit_index_ = 0;
  last_host_sample_bit_index_ = 0;

  // Reset DP/AP state.
  dp_ctrlstat_ = 0;
  dp_select_ = 0;
  dp_rdbuff_ = 0;
  ap_csw_ = 0;
  ap_tar_ = 0;

  flash_reset();
}

bool Stm32SwdTarget::consume_sampled_host_bit_flag() {
  const bool v = sampled_host_bit_;
  sampled_host_bit_ = false;
  return v;
}

uint8_t Stm32SwdTarget::shift_bit_count() const {
  // Expose whichever internal accumulator is currently active.
  // This is intentionally approximate / visualization-oriented.
  switch (phase_) {
    case Phase::CollectSeq:
      return seq_bits_;
    case Phase::CollectRequest:
      return req_bits_;
    case Phase::RecvData_Write:
      return write_bit_idx_;
    case Phase::RecvParity_Write:
      return 32;
    default:
      return 0;
  }
}

uint8_t Stm32SwdTarget::field_bit_index() const {
  // Return a field-local 1-based index intended to match SWD diagrams.
  switch (phase_) {
    case Phase::CollectSeq:
      // JTAG-to-SWD is not part of an SWD transaction; still useful to show 1..16.
      return (seq_bits_ == 0) ? 0 : seq_bits_;

    case Phase::CollectRequest:
      return (req_bits_ == 0) ? 0 : req_bits_;

    case Phase::TurnaroundToTarget_Read:
      // At this edge we present ACK bit0, but the host isn't sampling host-driven bits.
      return 0;

    case Phase::SendAck_Read:
    case Phase::SendAck_Write: {
      // bit_idx_ is the ACK bit being presented (1..2 while in SendAck_*)
      // ACK bit0 is presented in the preceding TurnaroundToTarget_* phase.
      // For visualization, we still want 1..3.
      if (bit_idx_ == 0) return 1;
      if (bit_idx_ == 1) return 2;
      if (bit_idx_ == 2) return 3;
      return 0;
    }

    case Phase::SendData_Read:
      // bit_idx_ counts 0..31 while presenting data bits.
      return (uint8_t)(bit_idx_ == 0 ? 1 : (bit_idx_));

    case Phase::SendParity_Read:
      return 33;

    case Phase::RecvData_Write:
      // write_bit_idx_ counts 0..31 of host-driven data bits.
      return (uint8_t)(write_bit_idx_ == 0 ? 1 : (write_bit_idx_));

    case Phase::RecvParity_Write:
      return 33;

    default:
      return 0;
  }
}

uint8_t Stm32SwdTarget::phase_id() const {
  return static_cast<uint8_t>(phase_);
}

const char *Stm32SwdTarget::phase_name() const {
  switch (phase_) {
    case Phase::AwaitResetOrSeq:
      return "AwaitResetOrSeq";
    case Phase::CollectSeq:
      return "CollectSeq";
    case Phase::CollectRequest:
      return "CollectRequest";
    case Phase::TurnaroundToTarget_Read:
      return "TurnaroundToTarget_Read";
    case Phase::SendAck_Read:
      return "SendAck_Read";
    case Phase::SendData_Read:
      return "SendData_Read";
    case Phase::SendParity_Read:
      return "SendParity_Read";
    case Phase::TurnaroundToHost_Read:
      return "TurnaroundToHost_Read";
    case Phase::TurnaroundToTarget_Write:
      return "TurnaroundToTarget_Write";
    case Phase::SendAck_Write:
      return "SendAck_Write";
    case Phase::TurnaroundToHost_Write:
      return "TurnaroundToHost_Write";
    case Phase::RecvData_Write:
      return "RecvData_Write";
    case Phase::RecvParity_Write:
      return "RecvParity_Write";
    case Phase::Complete_Write:
      return "Complete_Write";
    default:
      return "(unknown)";
  }
}

void Stm32SwdTarget::on_swclk_rising_edge(bool host_driving, uint8_t host_level) {
  sampled_host_bit_ = false;
  // Default to "not sampling" for this edge; set below when applicable.
  last_target_sample_bit_index_ = 0;
  // Host samples target-driven bits on SWCLK falling edge; however, for visualization we
  // can still expose which target-driven bit is being *presented* on this rising edge.
  last_host_sample_bit_index_ = 0;

  // Detect line reset: consecutive cycles where host drives SWDIO high.
  if (host_driving && host_level == 1) {
    consecutive_high_cycles_++;
  } else {
    consecutive_high_cycles_ = 0;
  }

  // Trigger a line reset event once when we first hit the threshold.
  // Using == avoids re-triggering on every subsequent high cycle.
  if (consecutive_high_cycles_ == 50) {
    line_reset_seen_ = true;
    post_reset_high_cycles_ = 0;
    drive_en_ = false;

    // Host emits line reset -> 0xE79E -> line reset.
    //
    // - Before we've seen 0xE79E: this line reset arms sequence capture.
    // - After we've seen 0xE79E: this line reset simply re-syncs in SWD mode and
    //   transactions can begin immediately (no second 0xE79E required).
    if (after_jtag_to_swd_) {
      // Stay in SWD mode; accept requests right away.
      phase_ = Phase::CollectRequest;
      req_shift_ = 0;
      req_bits_ = 0;
    } else {
      phase_ = Phase::AwaitResetOrSeq;
      seq_shift_ = 0;
      seq_bits_ = 0;
      req_shift_ = 0;
      req_bits_ = 0;
    }
  }

  switch (phase_) {
    case Phase::AwaitResetOrSeq: {
      // In pre-SWD mode, after a line reset the host will send 0xE79E (which begins with a 0).
      if (line_reset_seen_) {
        if (host_driving && host_level == 1) {
          post_reset_high_cycles_++;
          return;
        }
        // first 0 after reset: start capturing sequence
        line_reset_seen_ = false;
      }

      if (host_driving && host_level == 0) {
        phase_ = Phase::CollectSeq;
        seq_shift_ = 0;
        seq_bits_ = 0;
        // fallthrough to capture this bit
      } else {
        return;
      }
    }

    case Phase::CollectSeq: {
      if (!host_driving) return;
      sampled_host_bit_ = true;
      // JTAG-to-SWD sequence bits are 1..16
      last_target_sample_bit_index_ = (uint8_t)(seq_bits_ + 1);
      seq_shift_ |= (uint16_t)(host_level & 1u) << seq_bits_;
      seq_bits_++;
      if (seq_bits_ >= 16) {
        if (seq_shift_ == 0xE79E) {
          swd_enabled_ = true;
          after_jtag_to_swd_ = true;
          phase_ = Phase::CollectRequest;
          req_shift_ = 0;
          req_bits_ = 0;
        } else {
          // Wrong sequence; just keep hunting.
          phase_ = Phase::AwaitResetOrSeq;
        }
        line_reset_seen_ = false;
      }
      return;
    }

    case Phase::CollectRequest: {
      if (!host_driving) {
        return;
      }

      if (line_reset_seen_ && host_level == 1) return;
      if (line_reset_seen_ && host_level == 0) {
        line_reset_seen_ = false;
        req_shift_ = 0;
        req_bits_ = 0;
      }

      // allow idle-low bits before start
      if (req_bits_ == 0 && host_level == 0) return;

      sampled_host_bit_ = true;
      last_target_sample_bit_index_ = (uint8_t)(req_bits_ + 1);
      req_shift_ |= (uint8_t)(host_level & 1u) << req_bits_;
      req_bits_++;

      if (req_bits_ >= 8) {
        const uint8_t start = (req_shift_ >> 0) & 1u;
        const uint8_t apndp = (req_shift_ >> 1) & 1u;
        const uint8_t rnw = (req_shift_ >> 2) & 1u;
        const uint8_t a2 = (req_shift_ >> 3) & 1u;
        const uint8_t a3 = (req_shift_ >> 4) & 1u;
        const uint8_t par = (req_shift_ >> 5) & 1u;
        const uint8_t stop = (req_shift_ >> 6) & 1u;
        const uint8_t park = (req_shift_ >> 7) & 1u;

        const uint8_t parity_calc = (uint8_t)(apndp ^ rnw ^ a2 ^ a3);
        const uint8_t addr = (uint8_t)((a3 << 3) | (a2 << 2));
        const bool header_ok = (start == 1u) && (stop == 0u) && (park == 1u) && (parity_calc == par);

        if (!swd_enabled_ || !header_ok) {
          // ignore
          req_shift_ = 0;
          req_bits_ = 0;
          return;
        }

        req_addr_ = addr;
        if (apndp == 0u && rnw == 1u) req_kind_ = ReqKind::DpRead;
        else if (apndp == 0u && rnw == 0u) req_kind_ = ReqKind::DpWrite;
        else if (apndp == 1u && rnw == 1u) req_kind_ = ReqKind::ApRead;
        else req_kind_ = ReqKind::ApWrite;

        if (req_kind_ == ReqKind::DpRead || req_kind_ == ReqKind::ApRead) {
          // Prepare read value.
          if (req_kind_ == ReqKind::DpRead) {
            read_data_ = dp_read_reg(req_addr_);
          } else {
            // Posted read semantics: return stale buffer, then update dp_rdbuff with actual.
            const uint32_t actual = ap_read_reg(req_addr_);
            read_data_ = dp_rdbuff_;
            dp_rdbuff_ = actual;
          }
          read_parity_ = parity_u32(read_data_);

          phase_ = Phase::TurnaroundToTarget_Read;
          bit_idx_ = 0;

        } else {
          // Writes: target must ACK first, then host sends 32-bit data + parity.
          phase_ = Phase::TurnaroundToTarget_Write;
          write_data_ = 0;
          write_bit_idx_ = 0;
          write_parity_rx_ = 0;
        }

        req_shift_ = 0;
        req_bits_ = 0;
      }
      return;
    }

    // ===== Read response =====
    case Phase::TurnaroundToTarget_Read: {
      // Present ACK bit0 on this edge (matching host code's timing).
      const uint8_t ack_ok = 0b001;
      drive_en_ = true;
      drive_level_ = (ack_ok >> 0) & 1u;
      last_host_sample_bit_index_ = 1;
      phase_ = Phase::SendAck_Read;
      bit_idx_ = 1;
      return;
    }

    case Phase::SendAck_Read: {
      const uint8_t ack_ok = 0b001;
      drive_level_ = (ack_ok >> bit_idx_) & 1u;
      last_host_sample_bit_index_ = (uint8_t)(bit_idx_ + 1);
      bit_idx_++;
      if (bit_idx_ >= 3) {
        phase_ = Phase::SendData_Read;
        bit_idx_ = 0;
      }
      return;
    }

    case Phase::SendData_Read: {
      drive_level_ = get_bit_u32(read_data_, bit_idx_);
      last_host_sample_bit_index_ = (uint8_t)(bit_idx_ + 1);
      bit_idx_++;
      if (bit_idx_ >= 32) {
        phase_ = Phase::SendParity_Read;
      }
      return;
    }

    case Phase::SendParity_Read:
      drive_level_ = read_parity_;
      last_host_sample_bit_index_ = 33;
      phase_ = Phase::TurnaroundToHost_Read;
      return;

    case Phase::TurnaroundToHost_Read:
      drive_en_ = false;
      phase_ = Phase::CollectRequest;
      return;

    // ===== Write transaction =====
    case Phase::TurnaroundToTarget_Write: {
      // For writes, the target drives ACK during the turnaround period.
      const uint8_t ack_ok = 0b001;
      drive_en_ = true;
      drive_level_ = (ack_ok >> 0) & 1u;
      last_host_sample_bit_index_ = 1;
      phase_ = Phase::SendAck_Write;
      bit_idx_ = 1;
      return;
    }

    case Phase::SendAck_Write: {
      const uint8_t ack_ok = 0b001;
      drive_level_ = (ack_ok >> bit_idx_) & 1u;
      last_host_sample_bit_index_ = (uint8_t)(bit_idx_ + 1);
      bit_idx_++;
      if (bit_idx_ >= 3) {
        // After the last ACK bit is *presented* on this rising edge, we must keep driving
        // through the following falling edge so the host can sample it.
        // Release SWDIO on the *next* rising edge (edge-only model: target drive changes on ↑).
        phase_ = Phase::TurnaroundToHost_Write;
      }
      return;
    }

    case Phase::TurnaroundToHost_Write:
      // Target releases line ownership on this rising edge.
      // Host will begin driving later (in host code this corresponds to its extra turnaround clocks).
      drive_en_ = false;
      phase_ = Phase::RecvData_Write;
      return;

    case Phase::RecvData_Write: {
      if (!host_driving) return;
      sampled_host_bit_ = true;
      last_target_sample_bit_index_ = (uint8_t)(write_bit_idx_ + 1);
      write_data_ |= ((uint32_t)(host_level & 1u) << write_bit_idx_);
      write_bit_idx_++;
      if (write_bit_idx_ >= 32) {
        phase_ = Phase::RecvParity_Write;
      }
      return;
    }

    case Phase::RecvParity_Write: {
      if (!host_driving) return;
      sampled_host_bit_ = true;
      last_target_sample_bit_index_ = 33;
      write_parity_rx_ = (host_level & 1u);
      phase_ = Phase::Complete_Write;
      return;
    }

    case Phase::Complete_Write: {
      // Apply write if parity OK.
      const uint8_t p = parity_u32(write_data_);
      if (p == write_parity_rx_) {
        if (req_kind_ == ReqKind::DpWrite) {
          dp_write_reg(req_addr_, write_data_);
        } else if (req_kind_ == ReqKind::ApWrite) {
          ap_write_reg(req_addr_, write_data_);
        }
      }

      // Target does not drive anything for writes (no data response, only ACK in real SWD).
      // However the host implementation ignores WAIT/FAULT and just assumes OK.
      // For simplicity, we just go back to collecting requests.
      phase_ = Phase::CollectRequest;
      return;
    }

    default:
      return;
  }
}

} // namespace sim
