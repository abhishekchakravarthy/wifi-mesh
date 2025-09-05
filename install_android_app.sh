#!/bin/bash

# Android App Installation Script for ESP32-S3 Audio Echo
# Usage: ./install_android_app.sh

set -e  # Exit on any error

echo "Android App Installation for ESP32-S3 Audio Echo"
echo "================================================"

# Check if we're in the right directory
if [ ! -d "android" ]; then
    echo "❌ Android directory not found!"
    echo "Please run this script from the wifi-mesh root directory"
    exit 1
fi

# Check if Android SDK is available
if ! command -v adb &> /dev/null; then
    echo "❌ ADB not found. Please install Android SDK Platform Tools:"
    echo "   https://developer.android.com/studio/releases/platform-tools"
    exit 1
fi

# Check if device is connected
echo "🔍 Checking for connected Android devices..."
DEVICES=$(adb devices | grep -v "List of devices" | grep -v "^$" | wc -l)

if [ "$DEVICES" -eq 0 ]; then
    echo "❌ No Android devices found!"
    echo "Please:"
    echo "  1. Connect your Android device via USB"
    echo "  2. Enable USB Debugging in Developer Options"
    echo "  3. Allow USB Debugging when prompted"
    echo "  4. Run this script again"
    exit 1
fi

echo "📱 Found $DEVICES Android device(s)"

# Navigate to Android directory
cd android

# Check if Gradle wrapper exists
if [ ! -f "gradlew" ]; then
    echo "❌ Gradle wrapper not found!"
    echo "Please ensure you're in the correct Android project directory"
    exit 1
fi

# Make gradlew executable
chmod +x gradlew

echo "🔨 Building Android app..."
./gradlew assembleDebug

if [ $? -ne 0 ]; then
    echo "❌ Android build failed!"
    echo "Please check the build output above for errors"
    exit 1
fi

echo "✅ Android app built successfully!"

# Check if APK was created
APK_PATH="app/build/outputs/apk/debug/app-debug.apk"
if [ ! -f "$APK_PATH" ]; then
    echo "❌ APK file not found at: $APK_PATH"
    exit 1
fi

echo "📦 Installing APK to Android device..."
adb install -r "$APK_PATH"

if [ $? -eq 0 ]; then
    echo "✅ Android app installed successfully!"
    echo ""
    echo "🎉 ESP32-S3 Audio Echo Android app is ready!"
    echo ""
    echo "📱 App Details:"
    echo "  - Package: com.example.bleaudio"
    echo "  - Main Activity: MainActivity"
    echo "  - BLE Service: 12345678-1234-1234-1234-123456789ABC"
    echo "  - BLE Characteristic: 87654321-4321-4321-4321-CBA987654321"
    echo ""
    echo "🚀 To use the app:"
    echo "  1. Open the 'BLE Audio' app on your Android device"
    echo "  2. Make sure your ESP32-S3 is powered on and advertising"
    echo "  3. Tap 'Start Scan' to find 'ESP32S3-AudioEcho'"
    echo "  4. Tap on the device to connect"
    echo "  5. Tap 'Start Recording' to begin audio echo test"
    echo ""
    echo "📊 The app will:"
    echo "  - Capture audio from your microphone"
    echo "  - Send it to ESP32-S3 via BLE"
    echo "  - Receive the echo back from ESP32-S3"
    echo "  - Play the echoed audio through your speakers"
    echo ""
    echo "🔧 Troubleshooting:"
    echo "  - If connection fails, restart both ESP32-S3 and the app"
    echo "  - Check that ESP32-S3 is advertising as 'ESP32S3-AudioEcho'"
    echo "  - Ensure Bluetooth is enabled on your Android device"
    echo "  - Grant microphone and location permissions to the app"
else
    echo "❌ Android app installation failed!"
    echo "Please check the installation output above for errors"
    exit 1
fi
