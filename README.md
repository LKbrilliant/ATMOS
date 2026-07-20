# ATMOS
ATMOS is a ESP32 based weather display. 

## TODO
- Location data and min-max temperature range is hard coded. Need to add manual data entering to the captive portal

### Hardware
- Display: Waveshare ESP32-S3 2.8" non-touch
- 1GB micro SD card (FAT32) with the weather icons (.bin formatted) in its root directory.
- 3D printed enclosure (./CAD)
- 2000mAh lipo battery
- USB type C female connector
- Latching switch
- 4x M2-15mm mounting screws

### External libraries
- WiFi provisioner: https://github.com/MichMich/esp-idf-wifi-provisioner

### Custom fonts
- Used LVGL online font converter:  https://lvgl.io/tools/fontconverter
    - Bpp:4 bit-per-pixel
    - output format: C file
- Place the C file in the main.c directory
- in the font C file comment out "// .static_bitmap = 0" line. This gave me an error when uncommented.
- Add the font file name to "SRCS" list in CMakeLists.txt

### Weather icons
- Sticker style weather icons: https://icons8.com/icons/set/weather--style-stickers

### Image transform scripts
- src/scripts/resizer.py: Used to batch resize images in a given directory
- src/scripts/img_to_bin_lvgl_v8: Used to convert regular image files in to .bin files to load on display with minimum RAM usage.

# Notes

### Build instructions using ESP-IDF via VScode
- Started from the the manufacturer's (waveshare) demo files which uses LVGL v8
- Install ESP IDF for vs code - IDF version: v5.5.2
- Connect the USB cable to UART or USB port of the device
- Select flash method: UART or USB-JTAG based on the connected port
- Select the USB port using the detect button on VScode (this option is enabled via ESP IDF)
- Select the target: ESP32-S3 (via built in USB-JTAG)
- Do a 'Full clean'
- Then 'Build'

### Changes to the ESP-IDF environment
- Enable fonts
    - lv_font_montserrat_12
    - lv_font_montserrat_16
    - lv_font_montserrat_48
- Enable 'File system on top of posix API' needed for custom file system wrapper
    - Drive letter = '83' for letter S
    - working directory = /sdcard
- FAT Filesystem support -> Long file name support = 'long file name buffer in heap'
    - Note: Maybe this is why I had to implement a custom file wrapper. By default files were not opening may be due to file name length restriction.
    - Note: Could have put the images in the ESP32 instead of the SD card.

### Disclaimer
- Most of the code in this repository was generated with the assistance of AI. While reasonable effort has been made to review and test the generated code, it has not been formally audited. Use this software at your own risk, and verify its correctness and security before deploying it in production.
