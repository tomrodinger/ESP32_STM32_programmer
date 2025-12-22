import os

input_file = "bootloader_M17_hw1.5_scc3_1766404965.bin"
output_file = "src/binary.h"

if not os.path.exists(input_file):
    print(f"Error: {input_file} not found.")
    exit(1)

with open(input_file, "rb") as f:
    data = f.read()

with open(output_file, "w") as f:
    f.write("#ifndef BINARY_H\n")
    f.write("#define BINARY_H\n\n")
    f.write(f"const unsigned char firmware_bin[] = {{\n")
    
    for i, byte in enumerate(data):
        f.write(f"0x{byte:02X}, ")
        if (i + 1) % 16 == 0:
            f.write("\n")
            
    f.write("\n};\n\n")
    f.write(f"const unsigned int firmware_bin_len = {len(data)};\n\n")
    f.write("#endif\n")

print(f"Converted {input_file} to {output_file}")
