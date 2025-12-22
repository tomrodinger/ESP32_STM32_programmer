#include "swd_min.h"

namespace swd_min {

static Pins g_pins;
// User-facing verbose mode (toggled by 'd').
static bool g_verbose = true;

// Raw low-level packet tracing is intentionally OFF (too noisy for humans).
// If you need packet-level troubleshooting later, add a command to toggle this.
static constexpr bool k_verbose_raw = false;
static bool g_nrst_last_high = true;

void set_verbose(bool enabled) { g_verbose = enabled; }
bool verbose_enabled() { return g_verbose; }

#ifndef SWD_HALF_PERIOD_US
// Fast timing for tight window after NRST release.
// Original was 5µs (conservative), reduced to 1µs to fit more operations
// before target firmware can disable SWD pins.
#define SWD_HALF_PERIOD_US 1
#endif

static inline void swd_delay() { delayMicroseconds(SWD_HALF_PERIOD_US); }

static inline void swclk_low() {
  digitalWrite(g_pins.swclk, LOW);
}

static inline void swclk_high() {
  digitalWrite(g_pins.swclk, HIGH);
}

static inline void swdio_output() {
  pinMode(g_pins.swdio, OUTPUT);
}

static inline void swdio_input_pullup() {
  // During SWD turnaround the host must release SWDIO.
  // On the bench we use the ESP32 pull-down plus the target's pull-up to detect
  // that the line is truly released (mid-rail behavior).
  // NOTE: name kept as-is; simulator may override how INPUT_PULLDOWN is interpreted.
  pinMode(g_pins.swdio, INPUT_PULLDOWN);
}

static inline void swdio_write(uint8_t bit) {
  digitalWrite(g_pins.swdio, bit ? HIGH : LOW);
}

static inline uint8_t swdio_read() {
  return (uint8_t)digitalRead(g_pins.swdio);
}

static inline void pulse_clock() {
  // Edge model (see README):
  // - Target samples/updates on SWCLK rising edge.
  // - Host updates (when driving) and samples (when reading) on SWCLK falling edge.
  //
  // Implement a single clock period and *end exactly on the falling edge*.
  // This ensures any subsequent SWDIO drive change happens at the same timestamp
  // as the falling edge in the simulator waveform.
  swclk_low();
  swd_delay();
  swclk_high();
  swd_delay();
  swclk_low();
}

static inline void write_bit(uint8_t bit) {
  // Host driving rule (see README): host may only change SWDIO drive state on SWCLK ↓.
  //
  // We keep SWCLK idle-low between bits and we always *end* a bit on SWCLK ↓.
  // Therefore, at entry we are already aligned with the previous falling edge.
  // Apply the next SWDIO value immediately (simulator logs it at the same timestamp
  // as that falling edge), then generate the rising edge for the target to sample.
  swclk_low();
  swdio_write(bit);
  swd_delay();
  swclk_high();
  swd_delay();
  swclk_low();
}

static inline uint8_t read_bit() {
  // Host sampling rule (see README): host samples SWDIO on SWCLK ↓ when the target drives.
  // Target updates its driven SWDIO value on SWCLK ↑.
  //
  // So: create a rising edge first (target updates), then sample at the following falling edge.
  swclk_low();
  swd_delay();
  swclk_high();
  swd_delay();
  swclk_low();
  return swdio_read();
}

static inline uint8_t parity_u32(uint32_t v) {
  // Returns 1 for odd number of set bits.
  uint8_t p = 0;
  while (v) {
    p ^= 1;
    v &= (v - 1);
  }
  return p;
}

// Forward decls (needed because some inline helpers call these)
static inline void line_idle_cycles(uint32_t cycles);
static inline void line_idle_cycles_low(uint32_t cycles);

// Reserved for potential future use: explicit SWD idle cycles helper.
// (Keep unused to avoid warnings on some builds.)
// static inline void swd_line_idle_cycles(uint32_t cycles) { line_idle_cycles(cycles); }

static bool dp_read(uint8_t addr, uint32_t *val_out, uint8_t *ack_out, bool log_enable);
static bool dp_write(uint8_t addr, uint32_t val, uint8_t *ack_out, bool log_enable);
static bool ap_read(uint8_t addr, uint32_t *val_out, uint8_t *ack_out, bool log_enable);
static bool ap_write(uint8_t addr, uint32_t val, uint8_t *ack_out, bool log_enable);

static const char *dp_reg_name_read(uint8_t addr) {
  switch (addr) {
    case DP_ADDR_IDCODE: return "IDCODE";
    case DP_ADDR_CTRLSTAT: return "CTRL/STAT";
    case DP_ADDR_SELECT: return "SELECT";
    case DP_ADDR_RDBUFF: return "RDBUFF";
    default: return "(unknown)";
  }
}

static const char *dp_reg_name_write(uint8_t addr) {
  // Note: IDCODE and ABORT share the same address (0x00).
  // For writes, address 0x00 is always ABORT.
  if (addr == DP_ADDR_ABORT) return "ABORT";
  switch (addr) {
    case DP_ADDR_CTRLSTAT: return "CTRL/STAT";
    case DP_ADDR_SELECT: return "SELECT";
    default: return "(unknown)";
  }
}

static const char *ap_reg_name(uint8_t addr) {
  switch (addr) {
    case AP_ADDR_CSW: return "CSW";
    case AP_ADDR_TAR: return "TAR";
    case AP_ADDR_DRW: return "DRW";
    case AP_ADDR_IDR: return "IDR";
    default: return "(unknown)";
  }
}

static const char *dp_read_purpose(uint8_t addr) {
  switch (addr) {
    case DP_ADDR_IDCODE: return "Identify the target debug port";
    case DP_ADDR_CTRLSTAT: return "Check debug/system power-up handshake status";
    case DP_ADDR_SELECT: return "Confirm which AP/bank is selected";
    case DP_ADDR_RDBUFF: return "Fetch posted-read result (from previous AP read)";
    default: return "Read a debug-port register";
  }
}

static const char *dp_write_purpose(uint8_t addr, uint32_t val) {
  if (addr == DP_ADDR_ABORT) {
    if ((val & 0x1Eu) == 0x1Eu) return "Clear sticky error flags";
    return "Write ABORT to clear/abort debug-port errors";
  }
  if (addr == DP_ADDR_CTRLSTAT) {
    if ((val & 0x50000000u) == 0x50000000u) return "Request debug+system power-up";
    return "Write power/control bits";
  }
  if (addr == DP_ADDR_SELECT) {
    return "Select which Access Port (AP) and register bank to use";
  }
  return "Write a debug-port register";
}

static const char *ap_write_purpose(uint8_t addr, uint32_t val) {
  (void)val;
  switch (addr) {
    case AP_ADDR_CSW: return "Configure AHB-AP transfer settings";
    case AP_ADDR_TAR: return "Set the target memory address (TAR)";
    case AP_ADDR_DRW: return "Write a 32-bit word to target memory via AHB-AP";
    default: return "Write an AP register";
  }
}

static const char *ap_read_purpose(uint8_t addr) {
  switch (addr) {
    case AP_ADDR_DRW:
      return "Start a 32-bit memory read (posted; the true value will be read from DP RDBUFF)";
    case AP_ADDR_IDR: return "Read AHB-AP identification register";
    default: return "Read an AP register";
  }
}

// NOTE: Intentionally no raw per-bit / per-packet prints. We only print condensed,
// human-readable lines with: purpose + register + address + data + ACK.

static inline void line_idle_cycles(uint32_t cycles) {
  // Bus idle: host drives SWDIO high.
  swdio_output();
  swdio_write(1);
  for (uint32_t i = 0; i < cycles; i++) {
    pulse_clock();
  }
}

static inline void line_idle_cycles_low(uint32_t cycles) {
  // Bus idle/flush (low): host drives SWDIO low.
  // This is useful between transfers because it is unambiguous and cannot be
  // confused with the SWD line-reset sequence (which is triggered by long runs of 1s).
  swdio_output();
  swdio_write(0);
  for (uint32_t i = 0; i < cycles; i++) {
    pulse_clock();
  }
}

#ifndef SWD_POST_IDLE_LOW_CYCLES
// After each completed transfer, clock a short idle/flush window with SWDIO held LOW.
// This matches common probe waveforms and simplifies transaction boundaries.
#define SWD_POST_IDLE_LOW_CYCLES 8
#endif

static inline void line_reset() {
  // >50 cycles with SWDIO high.
  line_idle_cycles(80);
}

static inline void jtag_to_swd_sequence() {
  // Send 16-bit sequence 0xE79E, LSB-first.
  swdio_output();
  const uint16_t seq = 0xE79E;
  for (int i = 0; i < 16; i++) {
    write_bit((seq >> i) & 1);
  }
}

static inline uint8_t make_request(uint8_t apndp, uint8_t rnw, uint8_t addr) {
  // addr is byte address; use A[3:2] (bits 3..2).
  const uint8_t a2 = (addr >> 2) & 1;
  const uint8_t a3 = (addr >> 3) & 1;
  const uint8_t parity = (uint8_t)(apndp ^ rnw ^ a2 ^ a3);

  uint8_t req = 0;
  req |= 1u << 0;                 // Start
  req |= (apndp & 1u) << 1;       // APnDP
  req |= (rnw & 1u) << 2;         // RnW
  req |= (a2 & 1u) << 3;          // A2
  req |= (a3 & 1u) << 4;          // A3
  req |= (parity & 1u) << 5;      // Parity
  req |= 0u << 6;                 // Stop
  req |= 1u << 7;                 // Park
  return req;
}

static bool dp_read(uint8_t addr, uint32_t *val_out, uint8_t *ack_out, bool log_enable) {
  const uint8_t req = make_request(/*APnDP=*/0, /*RnW=*/1, addr);

  (void)req; // request byte is not printed (not useful for humans)

  // Request phase (host drives)
  swdio_output();
  swdio_write(1); // ensure high between transfers

#ifndef SWD_REQ_IDLE_LOW_BITS
  // Empirical quirk (seen with ST-LINK/V2 waveforms): insert idle-low bits
  // immediately before the request start bit.
  // Tunable so we can match real targets/probes.
#define SWD_REQ_IDLE_LOW_BITS 2
#endif

  for (int i = 0; i < (int)SWD_REQ_IDLE_LOW_BITS; i++) {
    write_bit(0);
  }

  for (int i = 0; i < 8; i++) {
    write_bit((req >> i) & 1);
  }

  // Turnaround: host releases SWDIO while SWCLK is low (end of last request bit).
  // The first ACK bit is sampled on the very next rising edge, so don't insert an
  // extra full clock here (it would delay sampling by one bit).
  swdio_input_pullup();

  // ACK (3 bits, LSB-first)
  uint8_t ack = 0;
  ack |= read_bit() << 0;
  ack |= read_bit() << 1;
  ack |= read_bit() << 2;
  if (ack_out) *ack_out = ack;

  if (k_verbose_raw) {
    Serial.printf("SWD DP READ  addr=0x%02X  ACK=%u (%s)\n", (unsigned)addr, (unsigned)ack,
                  ack_to_str(ack));
  }

  if (ack != ACK_OK) {
    // Make sure we get back to a known idle state.
    // Provide enough cycles for target->host turnaround in the edge-only model:
    // target releases on , host takes ownership 1.5 cycles later on .
    pulse_clock();
    pulse_clock();
    line_idle_cycles_low(SWD_POST_IDLE_LOW_CYCLES);
    return false;
  }

  // Data (32 bits, LSB-first) + parity
  uint32_t v = 0;
  for (int i = 0; i < 32; i++) {
    if (read_bit()) v |= (1u << i);
  }
  const uint8_t p_rx = read_bit();

  // Turnaround back: target releases, host drives
  // Edge-only model needs 1.5 cycles of Z from target->host turnaround.
  // See README: target releases on , host starts driving on the following .
  pulse_clock();
  pulse_clock();
  swdio_output();
  // Drive low for the post-transfer idle/flush window.
  swdio_write(0);

  // Parity check: SWD uses odd parity over the 32 data bits
  if (p_rx != parity_u32(v)) {
    if (k_verbose_raw) {
      Serial.printf("SWD DP READ  addr=0x%02X  PARITY FAIL  p_rx=%u p_calc=%u data=0x%08lX\n", (unsigned)addr,
                    (unsigned)p_rx, (unsigned)parity_u32(v), (unsigned long)v);
    }
    line_idle_cycles_low(SWD_POST_IDLE_LOW_CYCLES);
    return false;
  }

  if (k_verbose_raw) {
    Serial.printf("SWD DP READ  addr=0x%02X  data=0x%08lX  parity=%u\n", (unsigned)addr, (unsigned long)v,
                  (unsigned)p_rx);
  }

  if (val_out) *val_out = v;
  if (g_verbose && log_enable) {
    Serial.printf("%s (DP READ %s addr=0x%02X, data=0x%08lX, ACK=%u %s)\n", dp_read_purpose(addr),
                  dp_reg_name_read(addr), (unsigned)addr, (unsigned long)v, (unsigned)ack, ack_to_str(ack));
  }
  line_idle_cycles_low(SWD_POST_IDLE_LOW_CYCLES);
  return true;
}

static bool dp_write(uint8_t addr, uint32_t val, uint8_t *ack_out, bool log_enable) {
  const uint8_t req = make_request(/*APnDP=*/0, /*RnW=*/0, addr);

  (void)req; // request byte is not printed (not useful for humans)

  // Request phase
  swdio_output();
  swdio_write(1);

#ifndef SWD_REQ_IDLE_LOW_BITS
#define SWD_REQ_IDLE_LOW_BITS 2
#endif
  for (int i = 0; i < (int)SWD_REQ_IDLE_LOW_BITS; i++) {
    write_bit(0);
  }

  for (int i = 0; i < 8; i++) {
    write_bit((req >> i) & 1);
  }

  // Turnaround to target (host releases)
  swdio_input_pullup();

  uint8_t ack = 0;
  ack |= read_bit() << 0;
  ack |= read_bit() << 1;
  ack |= read_bit() << 2;
  if (ack_out) *ack_out = ack;

  if (k_verbose_raw) {
    Serial.printf("SWD DP WRITE addr=0x%02X  ACK=%u (%s)  data=0x%08lX\n", (unsigned)addr, (unsigned)ack,
                  ack_to_str(ack), (unsigned long)val);
  }

  if (ack != ACK_OK) {
    pulse_clock();
    pulse_clock();
    line_idle_cycles_low(SWD_POST_IDLE_LOW_CYCLES);
    return false;
  }

  // Turnaround back to host drive for data phase (1 cycle)
  // Edge-only model: target->host turnaround is 1.5 cycles.
  pulse_clock();
  pulse_clock();
  swdio_output();

  // Data + parity (host drives)
  for (int i = 0; i < 32; i++) {
    write_bit((val >> i) & 1u);
  }
  write_bit(parity_u32(val));

  // Post-transfer idle/flush (low)
  if (g_verbose && log_enable) {
    Serial.printf("%s (DP WRITE %s addr=0x%02X, data=0x%08lX, ACK=%u %s)\n", dp_write_purpose(addr, val),
                  dp_reg_name_write(addr), (unsigned)addr, (unsigned long)val, (unsigned)ack, ack_to_str(ack));
  }
  line_idle_cycles_low(SWD_POST_IDLE_LOW_CYCLES);
  return true;
}

static bool ap_read(uint8_t addr, uint32_t *val_out, uint8_t *ack_out, bool log_enable) {
  // AP reads are posted; caller should read DP RDBUFF to get the value.
  // We'll perform AP read request, then DP RDBUFF read.
  const uint8_t req = make_request(/*APnDP=*/1, /*RnW=*/1, addr);

  (void)req; // request byte is not printed (not useful for humans)

  swdio_output();
  swdio_write(1);
  for (int i = 0; i < (int)SWD_REQ_IDLE_LOW_BITS; i++) write_bit(0);
  for (int i = 0; i < 8; i++) write_bit((req >> i) & 1);

  swdio_input_pullup();
  uint8_t ack = 0;
  ack |= read_bit() << 0;
  ack |= read_bit() << 1;
  ack |= read_bit() << 2;
  if (ack_out) *ack_out = ack;

  if (k_verbose_raw) {
    Serial.printf("SWD AP READ  addr=0x%02X  ACK=%u (%s)\n", (unsigned)addr, (unsigned)ack, ack_to_str(ack));
  }

  if (ack != ACK_OK) {
    pulse_clock();
    pulse_clock();
    line_idle_cycles_low(SWD_POST_IDLE_LOW_CYCLES);
    return false;
  }

  // Turnaround to host drive; no data phase for AP read itself (target would drive data),
  // but in SWD protocol AP read returns data in same transaction. However we use the same
  // dp_read implementation already; simplest: perform full read here.
  // So instead of this custom path, just call dp_read-like? We'll implement properly below.

  // Data (32) + parity driven by target
  uint32_t v = 0;
  for (int i = 0; i < 32; i++) {
    if (read_bit()) v |= (1u << i);
  }
  const uint8_t p_rx = read_bit();

  // Turnaround back: target releases, host drives
  pulse_clock();
  pulse_clock();
  swdio_output();
  swdio_write(0);

  if (p_rx != parity_u32(v)) {
    if (k_verbose_raw) {
      Serial.printf("SWD AP READ  addr=0x%02X  PARITY FAIL  p_rx=%u p_calc=%u data=0x%08lX\n", (unsigned)addr,
                    (unsigned)p_rx, (unsigned)parity_u32(v), (unsigned long)v);
    }
    line_idle_cycles_low(SWD_POST_IDLE_LOW_CYCLES);
    return false;
  }

  if (k_verbose_raw) {
    Serial.printf("SWD AP READ  addr=0x%02X  data(stale)=0x%08lX  parity=%u\n", (unsigned)addr, (unsigned long)v,
                  (unsigned)p_rx);
  }

  // The value returned here is NOT the true AP register value (posted read semantics);
  // it is the stale read buffer. Caller should read DP RDBUFF.
  if (val_out) *val_out = v;
  if (g_verbose && log_enable) {
    Serial.printf("%s (AP READ %s addr=0x%02X, data(stale)=0x%08lX, ACK=%u %s)\n", ap_read_purpose(addr),
                  ap_reg_name(addr), (unsigned)addr, (unsigned long)v, (unsigned)ack, ack_to_str(ack));
  }
  line_idle_cycles_low(SWD_POST_IDLE_LOW_CYCLES);
  return true;
}

static bool ap_write_internal(uint8_t addr, uint32_t val, uint8_t *ack_out, bool log_enable, bool post_idle) {
  const uint8_t req = make_request(/*APnDP=*/1, /*RnW=*/0, addr);

  (void)req; // request byte is not printed (not useful for humans)

  swdio_output();
  swdio_write(1);
  for (int i = 0; i < (int)SWD_REQ_IDLE_LOW_BITS; i++) write_bit(0);
  for (int i = 0; i < 8; i++) write_bit((req >> i) & 1);

  swdio_input_pullup();
  uint8_t ack = 0;
  ack |= read_bit() << 0;
  ack |= read_bit() << 1;
  ack |= read_bit() << 2;
  if (ack_out) *ack_out = ack;

  if (k_verbose_raw) {
    Serial.printf("SWD AP WRITE addr=0x%02X  ACK=%u (%s)\n", (unsigned)addr, (unsigned)ack, ack_to_str(ack));
  }

  if (ack != ACK_OK) {
    pulse_clock();
    pulse_clock();
    // Get back to a known host-driven state.
    swdio_output();
    swdio_write(0);
    if (post_idle) line_idle_cycles_low(SWD_POST_IDLE_LOW_CYCLES);
    return false;
  }

  // Turnaround to host
  pulse_clock();
  pulse_clock();
  swdio_output();

  for (int i = 0; i < 32; i++) write_bit((val >> i) & 1u);
  write_bit(parity_u32(val));

  // Leave line in a known idle-low state.
  swdio_write(0);

  // Post-transfer idle/flush (low)
  if (g_verbose && log_enable) {
    Serial.printf("%s (AP WRITE %s addr=0x%02X, data=0x%08lX, ACK=%u %s)\n", ap_write_purpose(addr, val),
                  ap_reg_name(addr), (unsigned)addr, (unsigned long)val, (unsigned)ack, ack_to_str(ack));
  }
  if (post_idle) line_idle_cycles_low(SWD_POST_IDLE_LOW_CYCLES);
  return true;
}

static bool ap_write(uint8_t addr, uint32_t val, uint8_t *ack_out, bool log_enable) {
  return ap_write_internal(addr, val, ack_out, log_enable, /*post_idle=*/true);
}

void begin(const Pins &pins) {
  g_pins = pins;

  pinMode(g_pins.swclk, OUTPUT);
  swclk_low();

  pinMode(g_pins.nrst, OUTPUT);
  digitalWrite(g_pins.nrst, HIGH);
  g_nrst_last_high = true;

  swdio_output();
  swdio_write(1);
}

void release_swd_pins() {
  // Put SWD pins into high-impedance state so the target firmware can repurpose
  // them without fighting our GPIO drivers.
  //
  // NOTE: this intentionally does not touch NRST.
  pinMode(g_pins.swclk, INPUT);
  pinMode(g_pins.swdio, INPUT);
}

void set_nrst(bool asserted) {
  // asserted=true => drive NRST low
  const bool next_high = asserted ? false : true;
  if (next_high != g_nrst_last_high) {
    Serial.printf("---------------------------------------- NRST %s\n", next_high ? "HIGH" : "LOW");
    g_nrst_last_high = next_high;
  }
  digitalWrite(g_pins.nrst, asserted ? LOW : HIGH);
}

void set_nrst_quiet(bool asserted) {
  // asserted=true => drive NRST low
  const bool next_high = asserted ? false : true;
  g_nrst_last_high = next_high;
  digitalWrite(g_pins.nrst, asserted ? LOW : HIGH);
}

bool nrst_is_high() {
  return digitalRead(g_pins.nrst) == HIGH;
}

void reset_and_switch_to_swd() {
  // Hold target in reset during SWD attach. This matches ST-LINK/V2 behavior observed
  // on the bench (NRST held low across the early SWD connect + initial transactions).
  set_nrst(true);
  delay(20);

  // "Standard" SWD init: line reset -> JTAG-to-SWD -> line reset.
  // (While keeping NRST asserted.)
  line_reset();
  jtag_to_swd_sequence();
  line_reset();

  // A few extra idle cycles before first request.
  line_idle_cycles(16);

  // Intentionally do NOT release NRST here; keep it low through the IDCODE read.
}

void swd_line_reset() {
  // Perform SWD line reset + JTAG-to-SWD sequence WITHOUT touching NRST.
  // This is used to re-establish SWD communication after releasing NRST,
  // because on STM32G0 the system reset (NRST release) clears the DP/AP state.
  // See: Perplexity research on STM32G0 reset behavior.
  if (g_verbose) {
    Serial.println(
        "Re-sync SWD physical layer (line reset, JTAG-to-SWD sequence, line reset; NRST is not changed)");
  }
  line_reset();
  jtag_to_swd_sequence();
  line_reset();

  // Idle cycles to allow target to stabilize.
  line_idle_cycles(16);
}

bool connect_under_reset_and_init() {
  // "Connect under reset" sequence for targets that may disable SWD pins quickly.
  // This keeps SWD activity going while releasing NRST, then aggressively tries
  // to re-establish communication.
  //
  // Based on Perplexity research: "keep doing SWD transactions continuously,
  // then release NRST while the debugger is already active, and keep clocking SWD."

  if (g_verbose) {
    Serial.println("Connect-under-reset: aggressively re-connect to SWD immediately after releasing NRST");
  }

  // NRST should already be LOW at this point (from reset_and_switch_to_swd)
  // Release NRST while immediately starting SWD activity
  if (g_verbose) {
    Serial.println("Release reset and immediately re-sync SWD...");
  }
  set_nrst(false);

  // Immediately try multiple reconnect attempts with minimal delay
  for (int attempt = 0; attempt < 5; attempt++) {
    if (g_verbose) {
      Serial.printf("Reconnect attempt %d/5: line reset + JTAG-to-SWD + read DP IDCODE\n", attempt + 1);
    }
    // Quick line reset + JTAG-to-SWD
    line_reset();
    jtag_to_swd_sequence();
    line_reset();
    line_idle_cycles(8);

    // Try IDCODE read - if it works, we're connected
    uint32_t idcode = 0;
    uint8_t ack = 0;
    // Log this read in human format when verbose is enabled.
    if (dp_read(DP_ADDR_IDCODE, &idcode, &ack, /*log_enable=*/g_verbose) && ack == ACK_OK) {
      if (g_verbose) {
        Serial.printf("Re-connect success on attempt %d (DP IDCODE=0x%08lX)\n", attempt + 1, (unsigned long)idcode);
      }
      // Now do full DP init
      return dp_init_and_power_up();
    }

    if (g_verbose && attempt < 3) {
      Serial.printf("Re-connect attempt %d failed (ACK=%u %s); retrying...\n", attempt + 1, (unsigned)ack,
                    ack_to_str(ack));
    }

    // Very short delay before retry
    delayMicroseconds(100);
  }

  if (g_verbose) {
    Serial.println("Re-connect failed: no valid SWD response after releasing NRST");
  }
  return false;
}

bool attach_and_read_idcode(uint32_t *idcode_out, uint8_t *ack_out) {
  // The two banner lines requested should only appear as part of this convenience helper.
  if (g_verbose) {
    Serial.println("Assert reset and switch debug port to SWD mode (line reset + JTAG-to-SWD + line reset)");
  }
  reset_and_switch_to_swd();
  if (g_verbose) {
    Serial.println("SWD mode selected; NRST is still asserted (LOW)");
  }
  return read_idcode(idcode_out, ack_out);
}

bool read_idcode(uint32_t *idcode_out, uint8_t *ack_out) {
  // DP IDCODE is at address 0x00.
  // We treat this as a low-level primitive; callers decide whether to log.
  return dp_read(/*addr=*/0x00, idcode_out, ack_out, /*log_enable=*/false);
}

bool dp_read_reg(uint8_t addr, uint32_t *val_out, uint8_t *ack_out) {
  // Low-level primitive; callers decide whether to log.
  return dp_read(addr, val_out, ack_out, /*log_enable=*/false);
}

bool dp_write_reg(uint8_t addr, uint32_t val, uint8_t *ack_out) {
  // Low-level primitive; callers decide whether to log.
  return dp_write(addr, val, ack_out, /*log_enable=*/false);
}

bool dp_init_and_power_up() {
  // Minimal DP init for ADIv5:
  // - Clear sticky errors
  // - Request system + debug power up
  // - Wait for ack
  // - Configure lane (optional, ignored)

  if (g_verbose) {
    Serial.println("DP init: clear errors and request debug/system power-up");
  }

  // Bench observation: the first DP write after attach can fail unless we do a DP read first.
  // Do a harmless IDCODE read here to “prime” the link (no reset/attach performed).
  {
    uint8_t ack = 0;
    uint32_t idcode = 0;
    (void)dp_read(DP_ADDR_IDCODE, &idcode, &ack, /*log_enable=*/g_verbose);
  }

  // Clear sticky errors: write ABORT with STKCMPCLR/STKERRCLR/WDERRCLR/ORUNERRCLR
  // (bits 0..4: ORUNERRCLR=4, WDERRCLR=3, STKERRCLR=2, STKCMPCLR=1)
  {
    uint8_t ack = 0;
    (void)dp_write(DP_ADDR_ABORT, (1u << 4) | (1u << 3) | (1u << 2) | (1u << 1), &ack, /*log_enable=*/g_verbose);
  }

  // Power up request: CSYSPWRUPREQ (bit30) + CDBGPWRUPREQ (bit28)
  const uint32_t req = (1u << 30) | (1u << 28);
  {
    uint8_t ack = 0;
    if (!dp_write(DP_ADDR_CTRLSTAT, req, &ack, /*log_enable=*/g_verbose)) {
      if (g_verbose) {
        Serial.printf("DP power-up request failed (DP WRITE CTRL/STAT addr=0x%02X, data=0x%08lX, ACK=%u %s)\n",
                      (unsigned)DP_ADDR_CTRLSTAT, (unsigned long)req, (unsigned)ack, ack_to_str(ack));
      }
      return false;
    }
  }

  // Wait for CSYSPWRUPACK (bit31) and CDBGPWRUPACK (bit29)
  uint32_t last_cs = 0;
  bool have_last = false;
  for (int i = 0; i < 200; i++) {
    uint32_t cs = 0;
    uint8_t ack = 0;
    if (!dp_read(DP_ADDR_CTRLSTAT, &cs, &ack, /*log_enable=*/false)) {
      if (g_verbose && i < 10) {
        Serial.printf(
            "Poll CTRL/STAT failed (poll %d: DP READ CTRL/STAT addr=0x%02X, ACK=%u %s)\n", i,
            (unsigned)DP_ADDR_CTRLSTAT, (unsigned)ack, ack_to_str(ack));
      }
      continue;
    }
    const bool sys_ack = (cs >> 31) & 1u;
    const bool dbg_ack = (cs >> 29) & 1u;

    if (g_verbose) {
      const bool changed = (!have_last) || (cs != last_cs);
      if (i < 10 || changed) {
        Serial.printf(
            "Waiting for power-up ACKs (poll %d: DP READ CTRL/STAT addr=0x%02X, data=0x%08lX, ACK=%u %s, sys_ack=%u, dbg_ack=%u)\n",
            i, (unsigned)DP_ADDR_CTRLSTAT, (unsigned long)cs, (unsigned)ack, ack_to_str(ack), (unsigned)sys_ack,
            (unsigned)dbg_ack);
      }
      last_cs = cs;
      have_last = true;
    }

    if (sys_ack && dbg_ack) return true;
    delay(1);
  }
  if (g_verbose) {
    Serial.println("DP init timeout: never observed both SYS+DBG power-up ACK bits");
  }
  return false;
}

bool ap_select(uint8_t apsel, uint8_t apbanksel) {
  // SELECT: [31:24]=APSEL, [7:4]=APBANKSEL
  const uint32_t sel = ((uint32_t)apsel << 24) | ((uint32_t)(apbanksel & 0xFu) << 4);

  // Log in the requested one-line English format when verbose is enabled.
  return dp_write(DP_ADDR_SELECT, sel, nullptr, /*log_enable=*/g_verbose);
}

bool ap_read_reg(uint8_t addr, uint32_t *val_out, uint8_t *ack_out) {
  uint32_t dummy = 0;
  uint8_t ack0 = 0;
  if (!ap_read(addr, &dummy, &ack0, /*log_enable=*/false)) {
    if (ack_out) *ack_out = ack0;
    return false;
  }
  // True value in RDBUFF
  uint8_t ack1 = 0;
  uint32_t v = 0;
  if (!dp_read(DP_ADDR_RDBUFF, &v, &ack1, /*log_enable=*/false)) {
    if (ack_out) *ack_out = ack1;
    return false;
  }
  if (ack_out) *ack_out = ACK_OK;
  if (val_out) *val_out = v;

  // Intentionally do not print here; higher-level routines should log in human format.
  return true;
}

bool ap_write_reg(uint8_t addr, uint32_t val, uint8_t *ack_out) {
  uint8_t ack = 0;
  const bool ok = ap_write(addr, val, &ack, /*log_enable=*/false);
  if (ack_out) *ack_out = ack;
  return ok;
}

bool ap_write_reg_critical(uint8_t addr, uint32_t val, uint8_t *ack_out) {
  uint8_t ack = 0;
  const bool ok = ap_write_internal(addr, val, &ack, /*log_enable=*/false, /*post_idle=*/false);
  if (ack_out) *ack_out = ack;
  return ok;
}

bool mem_write32(uint32_t addr, uint32_t val) {
  // AHB-AP CSW value used throughout this repo (see MASS_ERASE.md / PC_READ.md).
  // Low bits still represent: SIZE=32-bit, AddrInc=single.
  // Extra upper bits are used by some probes/targets for robust access.
  const uint32_t CSW_32_INC = 0x23000012u;

  // Intentionally do not print raw memory ops here; callers should log in human format.
  if (!ap_select(/*apsel=*/0, /*apbanksel=*/0)) return false;
  if (!ap_write_reg(AP_ADDR_CSW, CSW_32_INC, nullptr)) return false;
  if (!ap_write_reg(AP_ADDR_TAR, addr, nullptr)) return false;
  if (!ap_write_reg(AP_ADDR_DRW, val, nullptr)) return false;
  return true;
}

bool mem_read32(uint32_t addr, uint32_t *val_out) {
  const uint32_t CSW_32_INC = 0x23000012u;

  // Intentionally do not print raw memory ops here; callers should log in human format.
  if (!ap_select(/*apsel=*/0, /*apbanksel=*/0)) return false;
  if (!ap_write_reg(AP_ADDR_CSW, CSW_32_INC, nullptr)) return false;
  if (!ap_write_reg(AP_ADDR_TAR, addr, nullptr)) return false;
  // Posted: initiate read from DRW then read RDBUFF
  uint32_t dummy = 0;
  if (!ap_read_reg(AP_ADDR_DRW, &dummy, nullptr)) return false;
  if (val_out) *val_out = dummy;
  return true;
}

bool mem_write32_verbose(const char *purpose, uint32_t addr, uint32_t val) {
  // AHB-AP CSW value used throughout this repo (see MASS_ERASE.md / PC_READ.md).
  const uint32_t CSW_32_INC = 0x23000012u;

  uint8_t ack = 0;
  if (!ap_select(/*apsel=*/0, /*apbanksel=*/0)) return false;

  // Log the underlying AP transactions as one-line English messages.
  const char *p = (purpose && purpose[0]) ? purpose : "Memory write";

  if (!ap_write(AP_ADDR_CSW, CSW_32_INC, &ack, /*log_enable=*/false)) return false;
  if (g_verbose) {
    Serial.printf("%s: Configure AHB-AP for 32-bit transfers (AP WRITE CSW addr=0x%02X, data=0x%08lX, ACK=%u %s)\n",
                  p, (unsigned)AP_ADDR_CSW, (unsigned long)CSW_32_INC, (unsigned)ack, ack_to_str(ack));
  }

  if (!ap_write(AP_ADDR_TAR, addr, &ack, /*log_enable=*/false)) return false;
  if (g_verbose) {
    Serial.printf("%s: Set target address (AP WRITE TAR addr=0x%02X, data=0x%08lX, ACK=%u %s)\n", p,
                  (unsigned)AP_ADDR_TAR, (unsigned long)addr, (unsigned)ack, ack_to_str(ack));
  }

  if (!ap_write(AP_ADDR_DRW, val, &ack, /*log_enable=*/false)) return false;
  if (g_verbose) {
    Serial.printf("%s: Write 32-bit value to target memory (AP WRITE DRW addr=0x%02X, data=0x%08lX, ACK=%u %s)\n", p,
                  (unsigned)AP_ADDR_DRW, (unsigned long)val, (unsigned)ack, ack_to_str(ack));
  }
  return true;
}

bool mem_read32_verbose(const char *purpose, uint32_t addr, uint32_t *val_out) {
  const uint32_t CSW_32_INC = 0x23000012u;

  uint8_t ack = 0;
  if (!ap_select(/*apsel=*/0, /*apbanksel=*/0)) return false;

  const char *p = (purpose && purpose[0]) ? purpose : "Memory read";

  if (!ap_write(AP_ADDR_CSW, CSW_32_INC, &ack, /*log_enable=*/false)) return false;
  if (g_verbose) {
    Serial.printf("%s: Configure AHB-AP for 32-bit transfers (AP WRITE CSW addr=0x%02X, data=0x%08lX, ACK=%u %s)\n",
                  p, (unsigned)AP_ADDR_CSW, (unsigned long)CSW_32_INC, (unsigned)ack, ack_to_str(ack));
  }

  if (!ap_write(AP_ADDR_TAR, addr, &ack, /*log_enable=*/false)) return false;
  if (g_verbose) {
    Serial.printf("%s: Set target address (AP WRITE TAR addr=0x%02X, data=0x%08lX, ACK=%u %s)\n", p,
                  (unsigned)AP_ADDR_TAR, (unsigned long)addr, (unsigned)ack, ack_to_str(ack));
  }

  // Start the posted read from DRW.
  uint32_t stale = 0;
  if (!ap_read(AP_ADDR_DRW, &stale, &ack, /*log_enable=*/false)) return false;
  if (g_verbose) {
    Serial.printf(
        "%s: Start a posted memory read (AP READ DRW addr=0x%02X, data(stale)=0x%08lX, ACK=%u %s)\n", p,
        (unsigned)AP_ADDR_DRW, (unsigned long)stale, (unsigned)ack, ack_to_str(ack));
  }

  // Fetch the true value via DP.RDBUFF.
  uint32_t v = 0;
  if (!dp_read(DP_ADDR_RDBUFF, &v, &ack, /*log_enable=*/false)) return false;
  if (g_verbose) {
    Serial.printf("%s: Fetch posted-read result (DP READ RDBUFF addr=0x%02X, data=0x%08lX, ACK=%u %s)\n", p,
                  (unsigned)DP_ADDR_RDBUFF, (unsigned long)v, (unsigned)ack, ack_to_str(ack));
  }

  if (val_out) *val_out = v;
  return true;
}

const char *ack_to_str(uint8_t ack) {
  switch (ack) {
    case ACK_OK: return "OK";
    case ACK_WAIT: return "WAIT";
    case ACK_FAULT: return "FAULT";
    default: return "(invalid)";
  }
}

} // namespace swd_min
