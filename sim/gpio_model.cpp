#include "gpio_model.h"

namespace sim {

void GpioModel::host_pinMode(int pin, PinDir dir, Pull pull) {
  auto &st = host_[pin];
  st.dir = dir;
  st.pull = pull;
}

void GpioModel::host_digitalWrite(int pin, uint8_t value) {
  auto &st = host_[pin];
  st.out = value ? 1 : 0;
  st.dir = PinDir::Output; // Arduino semantics: writing implies output
}

PinState GpioModel::host_state(int pin) const {
  auto it = host_.find(pin);
  if (it == host_.end()) return PinState{};
  return it->second;
}

void GpioModel::target_drive_swdio(bool enable, uint8_t value) {
  target_drive_en_ = enable;
  target_drive_val_ = value ? 1 : 0;
}

Resolved GpioModel::resolve_swdio(int swdio_pin) const {
  Resolved r;

  const PinState st = host_state(swdio_pin);
  const bool host_driving = (st.dir == PinDir::Output);
  const bool target_driving = target_drive_en_;

  if (host_driving && target_driving) {
    // Illegal contention: mark and make it obvious in the waveform.
    contention_seen_ = true;
    r.contention = true;
    r.voltage = 1.65;
    r.level = 1; // arbitrary; waveform is the important artifact here
    return r;
  }

  if (target_driving) {
    r.voltage = target_drive_val_ ? 3.2 : 0.1;
    r.level = target_drive_val_;
    return r;
  }

  if (host_driving) {
    r.voltage = st.out ? 3.3 : 0.0;
    r.level = st.out;
    return r;
  }

  // Host not driving: use pull state
  switch (st.pull) {
    case Pull::Down:
      r.voltage = 0.2;
      r.level = 0;
      break;
    case Pull::Up:
      r.voltage = 3.1;
      r.level = 1;
      break;
    case Pull::None:
    default:
      // Default to idle-high since SWD typically has a pull-up.
      r.voltage = 3.1;
      r.level = 1;
      break;
  }

  return r;
}

double GpioModel::resolve_host_pin_voltage(int pin) const {
  const PinState st = host_state(pin);
  if (st.dir == PinDir::Output) {
    return st.out ? 3.3 : 0.0;
  }
  // If a host pin is configured as input in our sim, treat as pulled-up idle.
  if (st.pull == Pull::Down) return 0.2;
  return 3.1;
}

} // namespace sim
