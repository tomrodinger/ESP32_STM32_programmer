#pragma once

#include "firmware_source.h"

#include <FS.h>

namespace firmware_source {

class FileReader final : public Reader {
 public:
  explicit FileReader(fs::FS &fs);
  ~FileReader() override;

  bool open(const char *path);
  void close();

  uint32_t size() const override;
  bool read_at(uint32_t offset, uint8_t *dst, uint32_t n, uint32_t *out_n) override;

 private:
  fs::FS &fs_;
  File f_;
  uint32_t size_ = 0;
};

}  // namespace firmware_source

