#pragma once

#include <cstdint>
#include <vector>

namespace sim {

// SWD target model:
// - Minimal DP/AP implementation sufficient for swd_min::dp_* and AHB-AP memory access.
// - Memory map includes a simulated STM32G0 flash array + flash controller registers.
class Stm32SwdTarget {
public:
  void reset();

  // Update simulated time (used for FLASH_SR.BSY timing).
  void set_time_ns(uint64_t t_ns) { t_ns_ = t_ns; }

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
  void set_idcode(uint32_t idcode) { dp_idcode_ = idcode; }

private:
  // --- SWD protocol state machine ---
  enum class Phase : uint8_t {
    AwaitResetOrSeq,
    CollectSeq,
    CollectRequest,

    // Common read response
    TurnaroundToTarget_Read,
    SendAck_Read,
    SendData_Read,
    SendParity_Read,
    TurnaroundToHost_Read,

    // Write transaction (target must ACK, then host sends data)
    TurnaroundToTarget_Write,
    SendAck_Write,
    TurnaroundToHost_Write,
    RecvData_Write,
    RecvParity_Write,
    Complete_Write,
  };

  enum class ReqKind : uint8_t { None, DpRead, DpWrite, ApRead, ApWrite };

  // Helpers
  static uint8_t parity_u32(uint32_t v);
  static inline uint8_t get_bit_u32(uint32_t v, uint8_t i) { return (v >> i) & 1u; }

  // --- DP/AP register model ---
  uint32_t dp_read_reg(uint8_t addr);
  void dp_write_reg(uint8_t addr, uint32_t v);

  uint32_t ap_read_reg(uint8_t addr);
  void ap_write_reg(uint8_t addr, uint32_t v);

  // --- AHB memory model ---
  bool mem_read32(uint32_t addr, uint32_t &out);
  bool mem_write32(uint32_t addr, uint32_t v);

  // --- STM32G0 flash controller simulation ---
  void flash_reset();
  void flash_update_busy();
  void flash_start_busy(uint64_t duration_ns);
  void flash_try_unlock(uint32_t key);
  void flash_start_mass_erase();
  void flash_program32(uint32_t addr, uint32_t v);

  // --- State ---
  uint64_t t_ns_ = 0;

  Phase phase_ = Phase::AwaitResetOrSeq;

  // line reset detection
  uint32_t consecutive_high_cycles_ = 0;
  bool line_reset_seen_ = false;
  uint32_t post_reset_high_cycles_ = 0;

  // JTAG-to-SWD sequence
  uint16_t seq_shift_ = 0;
  uint8_t seq_bits_ = 0;

  // Request collection
  uint8_t req_shift_ = 0;
  uint8_t req_bits_ = 0;
  bool swd_enabled_ = false;

  // Decoded request
  ReqKind req_kind_ = ReqKind::None;
  uint8_t req_addr_ = 0;

  // Read response payload
  uint32_t read_data_ = 0;
  uint8_t read_parity_ = 0;
  uint8_t bit_idx_ = 0;

  // Write receive payload
  uint32_t write_data_ = 0;
  uint8_t write_bit_idx_ = 0;
  uint8_t write_parity_rx_ = 0;

  // Target drive
  bool drive_en_ = false;
  uint8_t drive_level_ = 1;

  bool sampled_host_bit_ = false;

  // Host uses reset-and-switch sequence: line reset -> 0xE79E -> line reset.
  // Once we observe a valid 0xE79E, we expect one additional line reset (the one
  // the host sends immediately after switching). During that reset we should stay
  // in SWD mode and then accept requests.
  bool after_jtag_to_swd_ = false;

  // --- DP registers ---
  uint32_t dp_idcode_ = 0x0BC11477u;
  uint32_t dp_ctrlstat_ = 0;
  uint32_t dp_select_ = 0;
  uint32_t dp_rdbuff_ = 0;

  // --- AP registers (AHB-AP #0, bank 0 only for this sim) ---
  uint32_t ap_csw_ = 0;
  uint32_t ap_tar_ = 0;

  // --- Flash + flash regs ---
  std::vector<uint8_t> flash_;

  uint32_t flash_keyr_last_ = 0;
  uint32_t flash_sr_ = 0;
  uint32_t flash_cr_ = 0x80000000u; // LOCK set after reset

  uint64_t flash_bsy_clear_time_ns_ = 0;
};

} // namespace sim
