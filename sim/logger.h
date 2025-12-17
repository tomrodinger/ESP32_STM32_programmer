#pragma once

#include <cstdint>
#include <fstream>
#include <string>
#include <unordered_map>

namespace sim {

class CsvLogger {
public:
  explicit CsvLogger(const std::string &path);

  void log_voltage_change(uint64_t t_ns, const std::string &signal, double voltage);

private:
  std::ofstream out_;
  std::unordered_map<std::string, double> last_v_;
};

} // namespace sim
