#!/bin/bash

# ESP32 Flash Script for ESP32-S3-DevKitC-1
# This script helps flash the ESP32 with the BLE server code

echo "=== ESP32 Flash Script ==="
echo "Target: ESP32-S3-DevKitC-1"
echo ""

# Check if esptool is available
if ! command -v esptool.py &> /dev/null; then
    echo "Error: esptool.py not found. Please install it first:"
    echo "pip3 install esptool"
    exit 1
fi

# Check for USB devices
echo "Checking for USB devices..."
lsusb | grep -i esp || echo "No ESP32 devices found in USB list"

echo ""
echo "Available serial ports:"
ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null || echo "No serial devices found"

echo ""
echo "=== Manual Flashing Instructions ==="
echo "1. Connect your ESP32-S3-DevKitC-1 via USB"
echo "2. Hold the BOOT button and press RESET, then release BOOT"
echo "3. The ESP32 should enter download mode"
echo ""
echo "4. Run the following command (replace PORT with your device port):"
echo "   esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 921600 --before default_reset --after hard_reset write_flash 0x0 esp32_ble_server.bin"
echo ""
echo "5. Or use Arduino IDE:"
echo "   - Open Arduino IDE"
echo "   - Install ESP32 board package"
echo "   - Select Board: ESP32S3 Dev Module"
echo "   - Select Port: /dev/ttyACM0 (or your port)"
echo "   - Open esp32_ble_server.ino"
echo "   - Click Upload"
echo ""

# Try to detect ESP32 automatically
echo "=== Auto-detection ==="
for port in /dev/ttyACM* /dev/ttyUSB*; do
    if [ -e "$port" ]; then
        echo "Testing port: $port"
        if timeout 5 esptool.py --chip esp32s3 --port "$port" --baud 115200 chip_id &>/dev/null; then
            echo "✅ ESP32 found on $port"
            echo ""
            echo "To flash this device, run:"
            echo "esptool.py --chip esp32s3 --port $port --baud 921600 --before default_reset --after hard_reset write_flash 0x0 esp32_ble_server.bin"
            echo ""
            echo "Or use Arduino IDE with port: $port"
            break
        else
            echo "❌ No ESP32 on $port"
        fi
    fi
done

echo ""
echo "=== Troubleshooting ==="
echo "If you get permission errors:"
echo "sudo usermod -a -G dialout $USER"
echo "Then log out and log back in"
echo ""
echo "If the device isn't detected:"
echo "1. Try a different USB cable (some are power-only)"
echo "2. Try a different USB port"
echo "3. Make sure you're holding BOOT while pressing RESET"
echo ""
echo "For more help, see: https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/user_guide_v1.0.html"

