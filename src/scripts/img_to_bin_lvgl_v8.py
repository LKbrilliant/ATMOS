import os
import struct
import argparse
from PIL import Image

def convert_png_to_lvgl8_bin(input_path, output_path, use_alpha=True):
    # Open the image
    img = Image.open(input_path)
    img = img.convert("RGBA")  # Ensure we have RGBA values

    width, height = img.size

    if width > 2047 or height > 2047:
        raise ValueError(f"Image dimensions ({width}x{height}) exceed LVGL v8 limit of 2047px!")

    # 1. Generate the 4-byte LVGL v8 header
    # Format of lv_img_header_t in little-endian bitfield:
    # Bit 0-4: cf (5 bits) -> 4: TRUE_COLOR (RGB565), 5: TRUE_COLOR_ALPHA (RGB565+A)
    # Bit 5-7: always_zero (3 bits)
    # Bit 8-9: reserved (2 bits)
    # Bit 10-20: width (11 bits)
    # Bit 21-31: height (11 bits)

    cf = 5 if use_alpha else 4
    always_zero = 0
    reserved = 0

    # Pack the bitfields into a single 32-bit unsigned integer
    header_val = (cf & 0x1F)
    header_val |= (always_zero & 0x07) << 5
    header_val |= (reserved & 0x03) << 8
    header_val |= (width & 0x7FF) << 10
    header_val |= (height & 0x7FF) << 21

    header_bytes = struct.pack("<I", header_val)

    # 2. Process pixel data
    pixel_data = bytearray()

    for y in range(height):
        for x in range(width):
            r, g, b, a = img.getpixel((x, y))

            # Convert RGB888 to RGB565 (5 bits Red, 6 bits Green, 5 bits Blue)
            r5 = (r >> 3) & 0x1F
            g6 = (g >> 2) & 0x3F
            b5 = (b >> 3) & 0x1F

            rgb565 = (r5 << 11) | (g6 << 5) | b5

            # Standard ESP32 screens are little-endian.
            # (Note: If your colors end up swapped, you may need to swap these bytes or configure LV_COLOR_16_SWAP 1)
            pixel_bytes = struct.pack("<H", rgb565)
            pixel_data.extend(pixel_bytes)

            # If using transparency, append the 8-bit alpha byte directly after the RGB565 word
            if use_alpha:
                pixel_data.append(a)

    # Write header and raw bytes to the .bin file
    with open(output_path, "wb") as f:
        f.write(header_bytes)
        f.write(pixel_data)

    print(f"Successfully converted: {os.path.basename(input_path)} -> {os.path.basename(output_path)} ({width}x{height}, {'RGB565+Alpha' if use_alpha else 'RGB565'})")

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Convert PNG images to LVGL v8 compatible BIN files.")
    parser.add_argument("-i", "--input", required=True, help="Input PNG file or directory path")
    parser.add_argument("-o", "--output", help="Output BIN file or directory path")
    parser.add_argument("--no-alpha", action="store_true", help="Disable alpha channel (saves space if image is fully opaque)")

    args = parser.parse_args()
    use_alpha = not args.no_alpha

    if os.path.isdir(args.input):
        out_dir = args.output if args.output else args.input
        os.makedirs(out_dir, exist_ok=True)
        for file in os.listdir(args.input):
            if file.lower().endswith(".png"):
                inp = os.path.join(args.input, file)
                out = os.path.join(out_dir, os.path.splitext(file)[0] + ".bin")
                convert_png_to_lvgl8_bin(inp, out, use_alpha)
    else:
        out_file = args.output if args.output else os.path.splitext(args.input)[0] + ".bin"
        convert_png_to_lvgl8_bin(args.input, out_file, use_alpha)
