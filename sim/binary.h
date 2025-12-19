#pragma once

#include <cstdint>

// Host-simulator version of the firmware blob.
// Keep this in sync with the main project by running:
//   python3 convert_bin.py
// (that writes src/binary.h; you can copy it here if needed)

// For now, include the exact same bytes by including the generated header.
// This works because the simulator build includes ../src in include paths.
#include "../src/binary.h"
