#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>

namespace sim {

class CsvLogger {
public:
  explicit CsvLogger(const std::string &path);

  // Logs a value only if it changed since the last time this signal was logged.
  // Use this for step-wise voltage waveforms.
  void log_voltage_change(uint64_t t_ns, const std::string &signal, double voltage);

  // Logs a single point event (no de-dupe). Use this for sampling markers.
  void log_event(uint64_t t_ns, const std::string &signal, double value);

private:
  std::ofstream out_;
  std::unordered_map<std::string, double> last_v_;
};

} // namespace sim
