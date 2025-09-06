#!/bin/bash

echo "🎯 Audio Pipeline Debug Script"
echo "=============================="

# Check if device is connected
DEVICES=$(adb devices | grep "device$" | wc -l)
if [ $DEVICES -eq 0 ]; then
    echo "❌ No Android device connected. Please connect your device and enable USB debugging."
    exit 1
elif [ $DEVICES -gt 1 ]; then
    echo "⚠️ Multiple devices connected ($DEVICES devices)"
    echo "Available devices:"
    adb devices -l
    echo ""
    read -p "Enter device serial number (or press Enter to use first device): " device_serial
    if [ -z "$device_serial" ]; then
        DEVICE=$(adb devices | grep "device$" | head -1 | cut -f1)
        echo "Using first device: $DEVICE"
    else
        DEVICE=$device_serial
        echo "Using device: $DEVICE"
    fi
else
    DEVICE=$(adb devices | grep "device$" | cut -f1)
    echo "✅ Using device: $DEVICE"
fi

# Function to run logcat with specific filters
run_logcat() {
    local filter=$1
    local description=$2
    
    echo ""
    echo "🔍 $description"
    echo "Filter: $filter"
    echo "Press Ctrl+C to stop"
    echo "=============================="
    
    adb -s $DEVICE logcat -c  # Clear existing logs
    adb -s $DEVICE logcat -s $filter
}

# Menu for different debug options
echo ""
echo "Select debug option:"
echo "1. Basic Audio Pipeline (AudioCaptureManager, BLEAudioManager, OpusCodec)"
echo "2. Comprehensive Audio Debug (includes MainActivity)"
echo "3. Real-time Audio Monitoring (filtered for key events)"
echo "4. Error-Only Logging"
echo "5. Full Debug with Timestamps"
echo "6. Custom Filter"
echo ""

read -p "Enter choice (1-6): " choice

case $choice in
    1)
        run_logcat "AudioCaptureManager:D BLEAudioManager:D OpusCodec:D" "Basic Audio Pipeline Debug"
        ;;
    2)
        run_logcat "AudioCaptureManager:D BLEAudioManager:D OpusCodec:D OpusFrameFormat:D MainActivity:D" "Comprehensive Audio Debug"
        ;;
    3)
        echo "🔍 Real-time Audio Monitoring"
        echo "Press Ctrl+C to stop"
        echo "=============================="
        adb -s $DEVICE logcat -c
        adb -s $DEVICE logcat -s AudioCaptureManager:D BLEAudioManager:D OpusCodec:D | grep -E "(DECODING|PLAYING|ENCODED|RECEIVED|WM frame|Opus decode|Audio written)"
        ;;
    4)
        run_logcat "AudioCaptureManager:E BLEAudioManager:E OpusCodec:E OpusFrameFormat:E" "Error-Only Logging"
        ;;
    5)
        echo "🔍 Full Debug with Timestamps"
        echo "Press Ctrl+C to stop"
        echo "=============================="
        adb -s $DEVICE logcat -c
        adb -s $DEVICE logcat -v time -s AudioCaptureManager:D BLEAudioManager:D OpusCodec:D OpusFrameFormat:D
        ;;
    6)
        read -p "Enter custom filter (e.g., 'AudioCaptureManager:D BLEAudioManager:D'): " custom_filter
        run_logcat "$custom_filter" "Custom Filter Debug"
        ;;
    *)
        echo "❌ Invalid choice"
        exit 1
        ;;
esac
