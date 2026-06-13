#!/bin/bash
# test_tts_verify build script
# Run this in a terminal (not via WorkBuddy sandbox)

set -e

echo "=== TTS Reader Build ==="

# Source ESP-IDF environment
if [ -f ~/esp/esp-idf/export.sh ]; then
    source ~/esp/esp-idf/export.sh
elif [ -f ~/esp/esp-idf-v5.5.1/export.sh ]; then
    source ~/esp/esp-idf-v5.5.1/export.sh
else
    echo "ERROR: ESP-IDF not found at ~/esp/esp-idf/"
    exit 1
fi

echo "ESP-IDF sourced OK"

# Build
cd "$(dirname "$0")"
echo "Configuring..."
idf.py reconfigure

echo ""
echo "Building..."
idf.py build

echo ""
echo "=== Build complete ==="
echo "Flash command:  idf.py -p \$(ls /dev/cu.* | grep 'usbmodem\|wchusbserial\|usbserial' | head -1) flash"
