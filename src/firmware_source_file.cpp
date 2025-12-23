#include "firmware_source_file.h"

namespace firmware_source {

FileReader::FileReader(fs::FS &fs) : fs_(fs) {}

FileReader::~FileReader() { close(); }

bool FileReader::open(const char *path) {
  close();
  f_ = fs_.open(path, "r");
  if (!f_) return false;
  if (f_.isDirectory()) {
    close();
    return false;
  }
  size_ = (uint32_t)f_.size();
  return true;
}

void FileReader::close() {
  if (f_) f_.close();
  size_ = 0;
}

uint32_t FileReader::size() const { return size_; }

bool FileReader::read_at(uint32_t offset, uint8_t *dst, uint32_t n, uint32_t *out_n) {
  if (out_n) *out_n = 0;
  if (!f_) return false;
  if (!dst && n != 0) return false;
  if (offset > size_) return false;
  if (n == 0) return true;

  // Seek is required because the SWD routines will request offsets in ascending order.
  // (We could also keep current position and avoid seek, but seek is simplest and deterministic.)
  if (!f_.seek(offset, SeekSet)) return false;
  const int r = f_.read(dst, n);
  if (r < 0) return false;
  if (out_n) *out_n = (uint32_t)r;
  return true;
}

}  // namespace firmware_source

