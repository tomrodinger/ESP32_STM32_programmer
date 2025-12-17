#include "logger.h"

#include <iomanip>

namespace sim {

CsvLogger::CsvLogger(const std::string &path) : out_(path) {
  out_ << "t_ns,signal,voltage\n";
  out_.setf(std::ios::fixed);
  out_ << std::setprecision(3);
}

void CsvLogger::log_voltage_change(uint64_t t_ns, const std::string &signal, double voltage) {
  auto it = last_v_.find(signal);
  if (it != last_v_.end()) {
    if (it->second == voltage) return;
    it->second = voltage;
  } else {
    last_v_[signal] = voltage;
  }

  out_ << t_ns << "," << signal << "," << voltage << "\n";
}

void CsvLogger::log_event(uint64_t t_ns, const std::string &signal, double value) {
  out_ << t_ns << "," << signal << "," << value << "\n";
}

} // namespace sim
