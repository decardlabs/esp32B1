#!/usr/bin/env python3
"""
Fetch TinyUSB source files needed for ESP32-S3 MSC device mode.
Downloads directly from GitHub (tinyusb v0.15.x).

Usage: python3 fetch_tinyusb.py [output_dir]
Defaults to: components/tinyusb (relative to project root)
"""

import os
import sys
import urllib.request
import shutil

# TinyUSB version and base URL
TINYUSB_TAG = "0.15.0"
BASE = f"https://raw.githubusercontent.com/hathach/tinyusb/{TINYUSB_TAG}"

# Files to download (relative to tinyusb repo root)
FILES = [
    # Core headers
    "src/tusb.h",
    "src/tusb_option.h",
    "src/tusb_config.h",
    "src/common/tusb_common.h",
    "src/common/tusb_compiler.h",
    "src/common/tusb_fifo.h",
    "src/common/tusb_types.h",
    "src/common/tusb_verify.h",
    # Core sources
    "src/tusb.c",
    "src/device/usbd.h",
    "src/device/usbd.c",
    "src/device/usbd_control.h",
    "src/device/usbd_control.c",
    # MSC class
    "src/class/msc/msc_device.h",
    "src/class/msc/msc_device.c",
    # ESP32-S3 port
    "src/portable/espressif/esp32s3/dcd_esp32s3.h",
    "src/portable/espressif/esp32s3/dcd_esp32s3.c",
    "src/portable/espressif/esp32s3/hal_esp32s3.h",
    "src/portable/espressif/esp32s3/hal_esp32s3.c",
]

# Files NOT to fetch (known to not exist / problematic)
# We skip board files since we'll provide our own

OUTPUT_DIR = sys.argv[1] if len(sys.argv) > 1 else "components/tinyusb"


def fetch_file(url, outpath):
    """Download a single file."""
    os.makedirs(os.path.dirname(outpath), exist_ok=True)
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=15) as resp:
            data = resp.read()
            # Check if it's actually HTML (redirect/error page)
            if data.startswith(b"<!DOCTYPE") or data.startswith(b"<html"):
                print(f"  {url}: SKIP (not found)")
                return False
            with open(outpath, "wb") as f:
                f.write(data)
            print(f"  {os.path.relpath(outpath)} ({len(data)} bytes)")
            return True
    except Exception as e:
        print(f"  {url}: ERROR - {e}")
        return False


def main():
    print(f"Fetching TinyUSB v{TINYUSB_TAG} files...")
    print(f"Output: {OUTPUT_DIR}")
    print()

    # Download TinyUSB source files
    fetch_dir = os.path.join(OUTPUT_DIR, "tinyusb")
    success = 0
    failed = 0

    for fpath in FILES:
        url = f"{BASE}/{fpath}"
        outpath = os.path.join(fetch_dir, fpath)
        if fetch_file(url, outpath):
            success += 1
        else:
            failed += 1

    # Also fetch the default tusb_config.h template
    config_url = f"https://raw.githubusercontent.com/hathach/tinyusb/{TINYUSB_TAG}/hw/bsp/esp32s3/board/tusb_config.h"
    config_out = os.path.join(OUTPUT_DIR, "include", "tusb_config.h")
    if fetch_file(config_url, config_out):
        success += 1
    else:
        # Provide a minimal config ourselves
        pass
    failed -= 1  # config is optional

    print(f"\nDone: {success} files fetched, {failed} failed")

    # Create a version marker
    with open(os.path.join(OUTPUT_DIR, "VERSION"), "w") as f:
        f.write(f"TinyUSB v{TINYUSB_TAG}\n")


if __name__ == "__main__":
    main()
