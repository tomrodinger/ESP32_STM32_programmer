#include "swd_min.h"

namespace swd_min {

static Pins g_pins;

#ifndef SWD_HALF_PERIOD_US
// Conservative timing for jumper wires + Arduino digitalWrite/digitalRead.
#define SWD_HALF_PERIOD_US 5
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

static inline void swd_line_idle_cycles(uint32_t cycles) { line_idle_cycles(cycles); }

static bool dp_write(uint8_t addr, uint32_t val, uint8_t *ack_out);
static bool ap_read(uint8_t addr, uint32_t *val_out, uint8_t *ack_out);
static bool ap_write(uint8_t addr, uint32_t val, uint8_t *ack_out);

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

static bool dp_read(uint8_t addr, uint32_t *val_out, uint8_t *ack_out) {
  const uint8_t req = make_request(/*APnDP=*/0, /*RnW=*/1, addr);

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
    line_idle_cycles_low(SWD_POST_IDLE_LOW_CYCLES);
    return false;
  }

  if (val_out) *val_out = v;
  line_idle_cycles_low(SWD_POST_IDLE_LOW_CYCLES);
  return true;
}

static bool dp_write(uint8_t addr, uint32_t val, uint8_t *ack_out) {
  const uint8_t req = make_request(/*APnDP=*/0, /*RnW=*/0, addr);

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
  line_idle_cycles_low(SWD_POST_IDLE_LOW_CYCLES);
  return true;
}

static bool ap_read(uint8_t addr, uint32_t *val_out, uint8_t *ack_out) {
  // AP reads are posted; caller should read DP RDBUFF to get the value.
  // We'll perform AP read request, then DP RDBUFF read.
  const uint8_t req = make_request(/*APnDP=*/1, /*RnW=*/1, addr);

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
    line_idle_cycles_low(SWD_POST_IDLE_LOW_CYCLES);
    return false;
  }

  // The value returned here is NOT the true AP register value (posted read semantics);
  // it is the stale read buffer. Caller should read DP RDBUFF.
  if (val_out) *val_out = v;
  line_idle_cycles_low(SWD_POST_IDLE_LOW_CYCLES);
  return true;
}

static bool ap_write(uint8_t addr, uint32_t val, uint8_t *ack_out) {
  const uint8_t req = make_request(/*APnDP=*/1, /*RnW=*/0, addr);

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

  if (ack != ACK_OK) {
    pulse_clock();
    pulse_clock();
    line_idle_cycles_low(SWD_POST_IDLE_LOW_CYCLES);
    return false;
  }

  // Turnaround to host
  pulse_clock();
  pulse_clock();
  swdio_output();

  for (int i = 0; i < 32; i++) write_bit((val >> i) & 1u);
  write_bit(parity_u32(val));

  // Post-transfer idle/flush (low)
  line_idle_cycles_low(SWD_POST_IDLE_LOW_CYCLES);
  return true;
}

void begin(const Pins &pins) {
  g_pins = pins;

  pinMode(g_pins.swclk, OUTPUT);
  swclk_low();

  pinMode(g_pins.nrst, OUTPUT);
  digitalWrite(g_pins.nrst, HIGH);

  swdio_output();
  swdio_write(1);
}

void set_nrst(bool asserted) {
  // asserted=true => drive NRST low
  digitalWrite(g_pins.nrst, asserted ? LOW : HIGH);
}

void reset_and_switch_to_swd() {
  // Hold target in reset during SWD attach. This matches ST-LINK/V2 behavior observed
  // on the bench (NRST held low across the early SWD connect + initial transactions).
  digitalWrite(g_pins.nrst, LOW);
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

bool read_idcode(uint32_t *idcode_out, uint8_t *ack_out) {
  // DP IDCODE is at address 0x00.
  return dp_read(/*addr=*/0x00, idcode_out, ack_out);
}

bool dp_read_reg(uint8_t addr, uint32_t *val_out, uint8_t *ack_out) {
  return dp_read(addr, val_out, ack_out);
}

bool dp_write_reg(uint8_t addr, uint32_t val, uint8_t *ack_out) {
  return dp_write(addr, val, ack_out);
}

bool dp_init_and_power_up() {
  // Minimal DP init for ADIv5:
  // - Clear sticky errors
  // - Request system + debug power up
  // - Wait for ack
  // - Configure lane (optional, ignored)

  // Clear sticky errors: write ABORT with STKCMPCLR/STKERRCLR/WDERRCLR/ORUNERRCLR
  // (bits 0..4: ORUNERRCLR=4, WDERRCLR=3, STKERRCLR=2, STKCMPCLR=1)
  (void)dp_write_reg(DP_ADDR_ABORT, (1u << 4) | (1u << 3) | (1u << 2) | (1u << 1), nullptr);

  // Power up request: CSYSPWRUPREQ (bit30) + CDBGPWRUPREQ (bit28)
  const uint32_t req = (1u << 30) | (1u << 28);
  if (!dp_write_reg(DP_ADDR_CTRLSTAT, req, nullptr)) return false;

  // Wait for CSYSPWRUPACK (bit31) and CDBGPWRUPACK (bit29)
  for (int i = 0; i < 200; i++) {
    uint32_t cs = 0;
    if (!dp_read_reg(DP_ADDR_CTRLSTAT, &cs, nullptr)) continue;
    const bool sys_ack = (cs >> 31) & 1u;
    const bool dbg_ack = (cs >> 29) & 1u;
    if (sys_ack && dbg_ack) return true;
    delay(1);
  }
  return false;
}

bool ap_select(uint8_t apsel, uint8_t apbanksel) {
  // SELECT: [31:24]=APSEL, [7:4]=APBANKSEL
  const uint32_t sel = ((uint32_t)apsel << 24) | ((uint32_t)(apbanksel & 0xFu) << 4);
  return dp_write_reg(DP_ADDR_SELECT, sel, nullptr);
}

bool ap_read_reg(uint8_t addr, uint32_t *val_out, uint8_t *ack_out) {
  uint32_t dummy = 0;
  uint8_t ack0 = 0;
  if (!ap_read(addr, &dummy, &ack0)) {
    if (ack_out) *ack_out = ack0;
    return false;
  }
  // True value in RDBUFF
  uint8_t ack1 = 0;
  uint32_t v = 0;
  if (!dp_read_reg(DP_ADDR_RDBUFF, &v, &ack1)) {
    if (ack_out) *ack_out = ack1;
    return false;
  }
  if (ack_out) *ack_out = ACK_OK;
  if (val_out) *val_out = v;
  return true;
}

bool ap_write_reg(uint8_t addr, uint32_t val, uint8_t *ack_out) {
  uint8_t ack = 0;
  const bool ok = ap_write(addr, val, &ack);
  if (ack_out) *ack_out = ack;
  return ok;
}

bool mem_write32(uint32_t addr, uint32_t val) {
  // AHB-AP CSW: size=32-bit, AddrInc=single, DBGSTAT disabled.
  // CSW bits: [2:0] SIZE (0b010 for 32-bit), [5:4] AddrInc (0b01 for single)
  const uint32_t CSW_32_INC = (0b010u << 0) | (0b01u << 4);
  if (!ap_select(/*apsel=*/0, /*apbanksel=*/0)) return false;
  if (!ap_write_reg(AP_ADDR_CSW, CSW_32_INC, nullptr)) return false;
  if (!ap_write_reg(AP_ADDR_TAR, addr, nullptr)) return false;
  if (!ap_write_reg(AP_ADDR_DRW, val, nullptr)) return false;
  return true;
}

bool mem_read32(uint32_t addr, uint32_t *val_out) {
  const uint32_t CSW_32_INC = (0b010u << 0) | (0b01u << 4);
  if (!ap_select(/*apsel=*/0, /*apbanksel=*/0)) return false;
  if (!ap_write_reg(AP_ADDR_CSW, CSW_32_INC, nullptr)) return false;
  if (!ap_write_reg(AP_ADDR_TAR, addr, nullptr)) return false;
  // Posted: initiate read from DRW then read RDBUFF
  uint32_t dummy = 0;
  if (!ap_read_reg(AP_ADDR_DRW, &dummy, nullptr)) return false;
  if (val_out) *val_out = dummy;
  return true;
}

const __FlashStringHelper *ack_to_str(uint8_t ack) {
  switch (ack) {
    case ACK_OK: return F("OK");
    case ACK_WAIT: return F("WAIT");
    case ACK_FAULT: return F("FAULT");
    default: return F("(invalid)");
  }
}

} // namespace swd_min
