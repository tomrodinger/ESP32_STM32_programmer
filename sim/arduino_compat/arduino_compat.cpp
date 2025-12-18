#include "Arduino.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "../gpio_model.h"
#include "../logger.h"
#include "../stm32_swd_target.h"
#include "../sim_api.h"

namespace sim {

struct Runtime {
  uint64_t t_ns = 0;

  int swclk_pin = 35;
  int swdio_pin = 36;
  int nrst_pin  = 37;

  GpioModel gpio;
  CsvLogger logger{"signals.csv"};
  Stm32SwdTarget target;

  uint8_t last_swclk_level = 0;

  bool swdio_input_pullup_seen = false;
  bool target_drove_swdio_seen = false;
  bool target_voltage_logged_seen = false;

  Runtime() { target.reset(); }
};

Runtime &rt() {
  // Function-local static so destructors run at program exit.
  static Runtime r;
  return r;
}

static void log_all() {
  auto &r = rt();

  // SWCLK
  const double v_swclk = r.gpio.resolve_host_pin_voltage(r.swclk_pin);
  r.logger.log_voltage_change(r.t_ns, "SWCLK", v_swclk);

  // SWDIO
  const auto swdio = r.gpio.resolve_swdio(r.swdio_pin);
  r.logger.log_voltage_change(r.t_ns, "SWDIO", swdio.voltage);
  if (swdio.voltage == 0.1 || swdio.voltage == 3.2 || swdio.voltage == 1.65) {
    r.target_voltage_logged_seen = true;
  }

  // NRST
  const double v_nrst = r.gpio.resolve_host_pin_voltage(r.nrst_pin);
  r.logger.log_voltage_change(r.t_ns, "NRST", v_nrst);
}

static void maybe_clock_edge_update() {
  auto &r = rt();
  const sim::PinState st = r.gpio.host_state(r.swclk_pin);
  const uint8_t level = (st.dir == sim::PinDir::Output) ? st.out : 0;

  if (level == r.last_swclk_level) return;
  r.last_swclk_level = level;

  // Log after SWCLK change
  log_all();

  if (level == 1) {
    // Rising edge: inform target
    const sim::PinState hs = r.gpio.host_state(r.swdio_pin);
    const bool host_driving = (hs.dir == sim::PinDir::Output);
    const auto swdio = r.gpio.resolve_swdio(r.swdio_pin);

    // Update target based on what it sees.
    r.target.on_swclk_rising_edge(host_driving, swdio.level);

    // If the target sampled a host-driven bit at this edge, emit a marker event.
    if (r.target.consume_sampled_host_bit_flag()) {
      r.logger.log_event(r.t_ns, "SWDIO_SAMPLE_T", 3.42);
    }

    // Apply target driving decision into GPIO model.
    if (r.target.drive_enabled()) {
      r.gpio.target_drive_swdio(true, r.target.drive_level());
      r.target_drove_swdio_seen = true;
    } else {
      r.gpio.target_drive_swdio(false, 0);
    }

    // Log again in case target drive changed at this edge.
    log_all();
  }
}

bool contention_seen() {
  return rt().gpio.contention_seen();
}

bool swdio_input_pullup_seen() {
  return rt().swdio_input_pullup_seen;
}

bool target_drove_swdio_seen() {
  return rt().target_drove_swdio_seen;
}

bool target_voltage_logged_seen() {
  return rt().target_voltage_logged_seen;
}

} // namespace sim

// ===== Arduino API implementation =====

void pinMode(int pin, int mode) {
  auto &r = sim::rt();

  sim::PinDir dir = sim::PinDir::Input;
  sim::Pull pull = sim::Pull::None;

  switch (mode) {
    case OUTPUT:
      dir = sim::PinDir::Output;
      pull = sim::Pull::None;
      break;
    case INPUT_PULLUP:
      dir = sim::PinDir::Input;
      pull = sim::Pull::Up;
      if (pin == r.swdio_pin) r.swdio_input_pullup_seen = true;
      break;
    case INPUT_PULLDOWN:
      dir = sim::PinDir::Input;
      pull = sim::Pull::Down;
      // Keep the existing flag name but treat either pull as evidence that the host
      // released SWDIO during turnaround.
      if (pin == r.swdio_pin) r.swdio_input_pullup_seen = true;
      break;
    case INPUT:
    default:
      dir = sim::PinDir::Input;
      pull = sim::Pull::None;
      break;
  }

  r.gpio.host_pinMode(pin, dir, pull);
  sim::log_all();
}

void digitalWrite(int pin, int value) {
  auto &r = sim::rt();
  r.gpio.host_digitalWrite(pin, (value != 0) ? 1 : 0);
  sim::log_all();

  // If SWCLK toggled, run edge hooks.
  if (pin == r.swclk_pin) {
    sim::maybe_clock_edge_update();
  }
}

int digitalRead(int pin) {
  auto &r = sim::rt();
  if (pin == r.swdio_pin) {
    const auto swdio = r.gpio.resolve_swdio(r.swdio_pin);
    // Host sampling marker at the exact time the host reads SWDIO.
    r.logger.log_event(r.t_ns, "SWDIO_SAMPLE_H", 3.42);
    return swdio.level;
  }

  const sim::PinState st = r.gpio.host_state(pin);
  if (st.dir == sim::PinDir::Output) return st.out;

  // For inputs, use pull if present
  if (st.pull == sim::Pull::Down) return 0;
  return 1;
}

void delay(unsigned long ms) {
  auto &r = sim::rt();
  r.t_ns += (uint64_t)ms * 1000000ull;
  sim::log_all();
}

void delayMicroseconds(unsigned int us) {
  auto &r = sim::rt();
  r.t_ns += (uint64_t)us * 1000ull;
  sim::log_all();
}
