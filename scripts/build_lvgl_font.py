#!/usr/bin/env python3
import argparse
import os
import subprocess

from build_default_assets import get_lv_font_conv_command, get_symbols_from_language_json


def main():
    parser = argparse.ArgumentParser(description="Build an LVGL C font from a TTF file")
    parser.add_argument("--ttf", required=True, help="Input TTF font path")
    parser.add_argument("--output", required=True, help="Output C file path")
    parser.add_argument("--size", type=int, required=True, help="Font pixel size")
    parser.add_argument("--bpp", type=int, default=1, help="Bits per pixel")
    parser.add_argument("--language-json", help="Language JSON used when not building full range")
    parser.add_argument("--full-range", action="store_true", help="Include the full TTF glyph range")
    parser.add_argument("--range", dest="ranges", action="append",
        help="Unicode range to include, for example 0x20-0x7e. Can be passed multiple times.")
    args = parser.parse_args()

    if not os.path.exists(args.ttf):
        raise FileNotFoundError(f"TTF font not found: {args.ttf}")

    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    cmd = get_lv_font_conv_command(os.path.dirname(os.path.dirname(args.output))) + [
        "--force-fast-kern-format",
        "--no-compress",
        "--no-prefilter",
        "--font", args.ttf,
        "--format", "lvgl",
        "--lv-include", "lvgl.h",
        "--bpp", str(args.bpp),
        "-o", args.output,
        "--size", str(args.size),
    ]

    if args.full_range and args.ranges:
        raise ValueError("--full-range and --range cannot be used together")

    if args.full_range:
        cmd += ["-r", "0x0-0xfffff"]
        symbols_count = "full range"
    elif args.ranges:
        ranges = ",".join(args.ranges)
        cmd += ["-r", ranges]
        symbols_count = ranges
    else:
        if not args.language_json:
            raise ValueError("--language-json is required when --full-range is not set")
        symbols = get_symbols_from_language_json(args.language_json)
        cmd += ["--symbols", symbols]
        symbols_count = len(symbols)

    print(f"Building LVGL C font: {args.output}")
    print(f"  source: {args.ttf}")
    print(f"  size: {args.size}px, bpp: {args.bpp}, symbols: {symbols_count}")
    subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
