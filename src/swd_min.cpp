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
  // For this hardware debug session we intentionally enable a *pull-down* on the ESP32
  // so that, together with the target's external ~10k pull-up, SWDIO will sit mid-rail
  // if (and only if) the line is truly being released.
  //
  // NOTE: rename left as-is to keep call sites unchanged.
  pinMode(g_pins.swdio, INPUT_PULLDOWN);
}

static inline void swdio_write(uint8_t bit) {
  digitalWrite(g_pins.swdio, bit ? HIGH : LOW);
}

static inline uint8_t swdio_read() {
  return (uint8_t)digitalRead(g_pins.swdio);
}

static inline void pulse_clock() {
  // Keep SWCLK idle LOW between bits.
  swclk_low();
  swd_delay();
  swclk_high();
  swd_delay();
  swclk_low();
  swd_delay();
}

static inline void write_bit(uint8_t bit) {
  // Change data while SWCLK is low.
  swclk_low();
  swd_delay();
  swdio_write(bit);
  swd_delay();
  swclk_high();
  swd_delay();
  swclk_low();
  swd_delay();
}

static inline uint8_t read_bit() {
  // Sample on rising edge.
  swclk_low();
  swd_delay();
  swclk_high();
  swd_delay();
  uint8_t b = swdio_read();
  swclk_low();
  swd_delay();
  return b;
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

static inline void line_idle_cycles(uint32_t cycles) {
  // Bus idle: host drives SWDIO high.
  swdio_output();
  swdio_write(1);
  for (uint32_t i = 0; i < cycles; i++) {
    pulse_clock();
  }
}

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
    // Provide one extra cycle to allow target to release line, then re-drive high.
    pulse_clock();
    swdio_output();
    swdio_write(1);
    line_idle_cycles(8);
    return false;
  }

  // Data (32 bits, LSB-first) + parity
  uint32_t v = 0;
  for (int i = 0; i < 32; i++) {
    if (read_bit()) v |= (1u << i);
  }
  const uint8_t p_rx = read_bit();

  // Turnaround back: target releases, host drives
  pulse_clock();
  swdio_output();
  swdio_write(1);

  // Parity check: SWD uses odd parity over the 32 data bits
  if (p_rx != parity_u32(v)) {
    line_idle_cycles(8);
    return false;
  }

  if (val_out) *val_out = v;
  line_idle_cycles(8);
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

void reset_and_switch_to_swd() {
  // Hold target in reset during SWD attach. This matches ST-LINK/V2 behavior observed
  // on the bench (NRST held low across the early SWD connect + initial transactions).
  digitalWrite(g_pins.nrst, LOW);
  delay(20);

  // Alternate init preamble (per observed ST-LINK/V2 behavior and additional docs):
  // 1) Hold SWCLK high (~200us) while driving SWDIO low
  // 2) Release SWDIO and send ~16 clock pulses (SWDIO floating)
  // 3) Drive SWDIO high and send 30 clock cycles
  // 4) Send JTAG-to-SWD selection sequence (0xE79E, LSB-first)
  // 5) Send 50+ clock cycles with SWDIO held high again
  swclk_high();
  swdio_output();
  swdio_write(0);
  delayMicroseconds(200);

  // Release SWDIO (tristate) and clock.
  swdio_input_pullup();
  for (int i = 0; i < 16; i++) {
    pulse_clock();
  }

  // Drive SWDIO high and provide idle clocks.
  swdio_output();
  swdio_write(1);
  for (int i = 0; i < 30; i++) {
    pulse_clock();
  }

  jtag_to_swd_sequence();

  // 50+ cycles with SWDIO high.
  line_idle_cycles(80);

  // A few extra idle cycles before first request.
  line_idle_cycles(16);

  // Intentionally do NOT release NRST here; keep it low through the IDCODE read.
}

bool read_idcode(uint32_t *idcode_out, uint8_t *ack_out) {
  // DP IDCODE is at address 0x00.
  return dp_read(/*addr=*/0x00, idcode_out, ack_out);
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
