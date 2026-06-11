#!/usr/bin/env python3
"""Write a DIYFlasher-compatible catalog for the physical board bundles."""

import argparse
import json
from pathlib import Path


BOARDS = {
    "lilygo-t-display-s3": ("LilyGo T-Display S3", "0x0", 115200, True, False),
    "lilygo-t-display": ("LilyGo T-Display", "0x1000", 460800, False, True),
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("version")
    parser.add_argument("dist", type=Path)
    args = parser.parse_args()

    entries = []
    for board, (label, boot_address, baudrate, manual_bootloader, use_stub) in BOARDS.items():
        folder = f"{args.version}_{board}"
        bundle = args.dist / folder
        files = [
            (boot_address, "bootloader.bin"),
            ("0x8000", "partitions.bin"),
            ("0xE000", "boot_app0.bin"),
            ("0x10000", "firmware.bin"),
        ]
        for _, filename in files:
            if not (bundle / filename).is_file():
                parser.error(f"missing {bundle / filename}")
        entries.append({
            "value": folder,
            "label": f"{label} ({args.version})",
            "firmwareVersion": args.version,
            "board": label,
            "variants": [],
            "baudrate": baudrate,
            "manualBootloader": manual_bootloader,
            "useStub": use_stub,
            "projectUrl": "https://github.com/valerio-vaccaro/SerTun32",
            "files": [
                {"address": address, "url": f"{folder}/{filename}", "name": filename}
                for address, filename in files
            ],
        })

    output = args.dist / "firmwares-sertun32.json"
    output.write_text(json.dumps(entries, indent=2) + "\n", encoding="utf-8")
    print(output)


if __name__ == "__main__":
    main()
