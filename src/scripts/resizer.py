import os
import argparse
from PIL import Image

def calculate_dimensions(original_width, original_height, target_width=None, target_height=None):
    """
    Calculates new dimensions maintaining the original aspect ratio.
    - If both are provided, works as a maximum bounding box.
    - If only one is provided, calculates the other proportionally.
    """
    if target_width and target_height:
        # Both provided: target_width and target_height act as a maximum bounding box
        ratio = min(target_width / original_width, target_height / original_height)
        # Avoid upscaling if the image is already smaller than the bounding box
        if ratio >= 1.0:
            return original_width, original_height
        return int(original_width * ratio), int(original_height * ratio)

    elif target_width:
        # Only width provided: calculate proportional height
        ratio = target_width / original_width
        return target_width, int(original_height * ratio)

    elif target_height:
        # Only height provided: calculate proportional width
        ratio = target_height / original_height
        return int(original_width * ratio), target_height

    # Fallback if somehow neither is passed (shouldn't happen due to parser validation)
    return original_width, original_height

def resize_images(input_folder, output_folder, target_width, target_height):
    """Resizes all valid images in input_folder and saves them to output_folder."""
    if not os.path.exists(output_folder):
        os.makedirs(output_folder)
        print(f"Created output directory: {output_folder}")

    valid_extensions = ('.jpg', '.jpeg', '.png', '.bmp', '.webp', '.tiff')
    success_count = 0
    fail_count = 0

    for filename in os.listdir(input_folder):
        if filename.lower().endswith(valid_extensions):
            input_path = os.path.join(input_folder, filename)
            output_path = os.path.join(output_folder, filename)

            try:
                with Image.open(input_path) as img:
                    orig_w, orig_h = img.size

                    # Calculate target dimensions dynamically based on user input
                    new_w, new_h = calculate_dimensions(orig_w, orig_h, target_width, target_height)

                    # Perform high-quality resizing
                    resized_img = img.resize((new_w, new_h), Image.Resampling.LANCZOS)
                    resized_img.save(output_path)

                    print(f"✓ Resized '{filename}': ({orig_w}x{orig_h}) -> ({new_w}x{new_h})")
                    success_count += 1
            except Exception as e:
                print(f"✗ Failed to process {filename}: {e}")
                fail_count += 1

    print(f"\nDone! Successfully processed {success_count} images. Failed: {fail_count}.")

def main():
    parser = argparse.ArgumentParser(
        description="Batch resize images with strict aspect ratio preservation using explicit flags."
    )

    # Required named arguments (using explicit flags)
    parser.add_argument("-i", "--input", required=True, help="Path to the input directory containing original images")
    parser.add_argument("-o", "--output", required=True, help="Path to the output directory where resized images will be saved")

    # Optional dimension arguments (neither has a default, allowing us to detect if only one is passed)
    parser.add_argument("-w", "--width", type=int, help="Target width in pixels")
    parser.add_argument("-t", "--height", type=int, help="Target height in pixels")

    args = parser.parse_args()

    # Fallback check: If the user specified neither width nor height, default to 800x800
    if args.width is None and args.height is None:
        print("No dimensions specified. Defaulting to a maximum bounding box of 800x800 pixels.\n")
        width, height = 800, 800
    else:
        width, height = args.width, args.height

    resize_images(args.input, args.output, width, height)

if __name__ == "__main__":
    main()
