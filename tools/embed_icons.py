import os
import sys

def main():
    if len(sys.argv) < 3:
        print("Usage: embed_icons.py <output_file> <input_dir>")
        sys.exit(1)

    output_file = sys.argv[1]
    input_dir = sys.argv[2]

    os.makedirs(os.path.dirname(output_file), exist_ok=True)

    with open(output_file, 'w') as f:
        f.write("#pragma once\n\n")
        f.write("#include <cstddef>\n")
        f.write("#include <cstdint>\n\n")
        f.write("namespace eruption::icons {\n\n")

        png_files = [f for f in os.listdir(input_dir) if f.endswith('.png')]
        png_files.sort()

        for png in png_files:
            name = os.path.splitext(png)[0]
            with open(os.path.join(input_dir, png), 'rb') as img:
                data = img.read()
            
            f.write(f"constexpr uint8_t {name}_png[] = {{\n")
            
            # Format nicely
            hex_data = [f"0x{b:02x}" for b in data]
            for i in range(0, len(hex_data), 12):
                f.write("    " + ", ".join(hex_data[i:i+12]) + ",\n")
            
            f.write("};\n")
            f.write(f"constexpr size_t {name}_png_len = {len(data)};\n\n")
            
        f.write("} // namespace eruption::icons\n")

if __name__ == "__main__":
    main()
