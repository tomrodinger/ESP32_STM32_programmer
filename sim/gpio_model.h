#pragma once

#include <cstdint>
#include <unordered_map>

namespace sim {

enum class PinDir : uint8_t { Input = 0, Output = 1 };

enum class Pull : uint8_t { None = 0, Up = 1, Down = 2 };

struct PinState {
  PinDir dir = PinDir::Input;
  Pull pull = Pull::None;
  uint8_t out = 0; // valid when dir==Output
};

struct Resolved {
  double voltage = 3.1;   // default idle-high pullup-ish
  uint8_t level = 1;      // logic level as seen by digitalRead
  bool contention = false;
};

// Models host GPIO state and resolves SWDIO/SWCLK/NRST voltages.
class GpioModel {
public:
  void host_pinMode(int pin, PinDir dir, Pull pull);
  void host_digitalWrite(int pin, uint8_t value);

  PinState host_state(int pin) const;

  // Target can only drive SWDIO in this project.
  void target_drive_swdio(bool enable, uint8_t value);

  // Resolve the SWDIO line (voltage + digital level) for logging and digitalRead.
  Resolved resolve_swdio(int swdio_pin) const;

  // Resolve a host-driven pin (SWCLK/NRST) voltage.
  double resolve_host_pin_voltage(int pin) const;

  bool contention_seen() const { return contention_seen_; }
  void clear_contention_seen() { contention_seen_ = false; }

private:
  std::unordered_map<int, PinState> host_;

  bool target_drive_en_ = false;
  uint8_t target_drive_val_ = 0;

  mutable bool contention_seen_ = false;
};

} // namespace sim
