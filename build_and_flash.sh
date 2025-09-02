#!/bin/bash

# ESP32-S3 BLE Audio Server Build and Flash Script
# Supports both ESP-IDF and PlatformIO build systems

echo "=== ESP32-S3 BLE Audio Server Build and Flash Script ==="
echo ""

# Check if ESP-IDF is available
if [ -n "$IDF_PATH" ] || command -v idf.py &> /dev/null; then
    echo "✅ ESP-IDF detected"
    BUILD_SYSTEM="ESP-IDF"
elif command -v pio &> /dev/null; then
    echo "✅ PlatformIO detected"
    BUILD_SYSTEM="PlatformIO"
else
    echo "❌ Neither ESP-IDF nor PlatformIO found"
    echo ""
    echo "Please install one of the following:"
    echo "1. ESP-IDF: https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/"
    echo "2. PlatformIO: pip install platformio"
    exit 1
fi

# Check for ESP32 device
echo ""
echo "Checking for ESP32 device..."
if ls /dev/ttyACM* &>/dev/null; then
    ESP32_PORT=$(ls /dev/ttyACM* | head -1)
    echo "✅ ESP32 found on $ESP32_PORT"
else
    echo "❌ No ESP32 device found"
    echo "Please connect your ESP32-S3-DevKitC-1 and try again"
    exit 1
fi

echo ""
echo "=== Building with $BUILD_SYSTEM ==="

# Build the project
if [ "$BUILD_SYSTEM" = "ESP-IDF" ]; then
    echo "Building with ESP-IDF..."
    if idf.py build; then
        echo "✅ Build successful!"
    else
        echo "❌ Build failed!"
        exit 1
    fi
elif [ "$BUILD_SYSTEM" = "PlatformIO" ]; then
    echo "Building with PlatformIO..."
    if pio run; then
        echo "✅ Build successful!"
    else
        echo "❌ Build failed!"
        exit 1
    fi
fi

echo ""
echo "=== Flashing ESP32 ==="
echo "Target: $ESP32_PORT"
echo ""

# Ask user if they want to flash
read -p "Do you want to flash the ESP32 now? (y/n): " -n 1 -r
echo ""
if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Put ESP32 in download mode:"
    echo "1. Hold the BOOT button"
    echo "2. Press and release the RESET button"
    echo "3. Release the BOOT button"
    echo ""
    read -p "Press Enter when ready..."
    
    # Flash the ESP32
    if [ "$BUILD_SYSTEM" = "ESP-IDF" ]; then
        echo "Flashing with ESP-IDF..."
        if idf.py -p $ESP32_PORT flash; then
            echo "✅ Flash successful!"
        else
            echo "❌ Flash failed!"
            exit 1
        fi
    elif [ "$BUILD_SYSTEM" = "PlatformIO" ]; then
        echo "Flashing with PlatformIO..."
        if pio run --target upload --upload-port $ESP32_PORT; then
            echo "✅ Flash successful!"
        else
            echo "❌ Flash failed!"
            exit 1
        fi
    fi
    
    echo ""
    echo "=== Monitoring ESP32 ==="
    echo "Press Ctrl+C to stop monitoring"
    echo ""
    
    # Monitor the ESP32
    if [ "$BUILD_SYSTEM" = "ESP-IDF" ]; then
        idf.py -p $ESP32_PORT monitor
    elif [ "$BUILD_SYSTEM" = "PlatformIO" ]; then
        pio device monitor --port $ESP32_PORT
    fi
else
    echo "Build completed. To flash later, run:"
    if [ "$BUILD_SYSTEM" = "ESP-IDF" ]; then
        echo "idf.py -p $ESP32_PORT flash monitor"
    elif [ "$BUILD_SYSTEM" = "PlatformIO" ]; then
        echo "pio run --target upload --upload-port $ESP32_PORT"
        echo "pio device monitor --port $ESP32_PORT"
    fi
fi

echo ""
echo "=== Build Artifacts ==="
if [ "$BUILD_SYSTEM" = "ESP-IDF" ]; then
    echo "Binary: build/esp32_ble_server.bin"
    echo "ELF: build/esp32_ble_server.elf"
elif [ "$BUILD_SYSTEM" = "PlatformIO" ]; then
    echo "Binary: .pio/build/esp32-s3-devkitc-1/firmware.bin"
    echo "ELF: .pio/build/esp32-s3-devkitc-1/firmware.elf"
fi

