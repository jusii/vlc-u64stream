#!/usr/bin/env python3
"""
u64_test_sender.py — synthesise an Ultimate-64 video UDP stream for testing.

Sends 384x272 PAL (or 384x240 NTSC) frames at 50/60 Hz to udp://host:port using
the exact wire format the U64 firmware emits, so the vlc-u64stream plugin can
be exercised without real hardware.

Usage:
    python3 tools/u64_test_sender.py [--host 127.0.0.1] [--port 11000]
                                     [--ntsc] [--fps N]

Press Ctrl-C to stop.
"""

from __future__ import annotations

import argparse
import math
import socket
import struct
import sys
import time

PIXELS_PER_LINE = 384
LINES_PER_PACKET = 4
PAYLOAD_BYTES = (PIXELS_PER_LINE * LINES_PER_PACKET) // 2  # 4-bit packed = 768

PAL_HEIGHT = 272
NTSC_HEIGHT = 240


def build_test_frame(height: int, frame_no: int) -> bytes:
    """Return an indexed-color frame (height x 384 bytes, one nibble/pixel)."""
    buf = bytearray(height * PIXELS_PER_LINE)
    # Vertical color bars in the C64 palette (16 colors).
    bar_w = PIXELS_PER_LINE // 16
    for y in range(height):
        row_off = y * PIXELS_PER_LINE
        for x in range(PIXELS_PER_LINE):
            # Color = horizontal bar index, with a slow vertical animation.
            base = (x // bar_w) & 0x0F
            # Animate: a sine band overlays a moving stripe in white(1)/black(0).
            band = math.sin((y + frame_no) * 0.05) * 8
            if abs((y % 32) - 16 + band) < 1.5:
                base = 1  # white scanline ribbon
            buf[row_off + x] = base
    # Bottom rows: solid black border so it's obvious if rows are misaligned.
    for y in range(height - 4, height):
        for x in range(PIXELS_PER_LINE):
            buf[y * PIXELS_PER_LINE + x] = 0
    return bytes(buf)


def pack_lines(frame: bytes, y0: int, n_lines: int) -> bytes:
    """Pack n_lines starting at row y0 into nibble-packed payload bytes.
       Lower nibble = first/leftmost pixel."""
    out = bytearray(PAYLOAD_BYTES)
    for row in range(n_lines):
        src = (y0 + row) * PIXELS_PER_LINE
        dst = row * (PIXELS_PER_LINE // 2)
        for xb in range(PIXELS_PER_LINE // 2):
            p0 = frame[src + xb * 2 + 0] & 0x0F
            p1 = frame[src + xb * 2 + 1] & 0x0F
            out[dst + xb] = p0 | (p1 << 4)
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=11000)
    ap.add_argument("--ntsc", action="store_true",
                    help="Emit 384x240 @ 60Hz instead of 384x272 @ 50Hz")
    ap.add_argument("--fps", type=float, default=None,
                    help="Override frame rate (default 50/60 from PAL/NTSC)")
    args = ap.parse_args()

    height = NTSC_HEIGHT if args.ntsc else PAL_HEIGHT
    fps = args.fps if args.fps else (60.0 if args.ntsc else 50.0)
    frame_period = 1.0 / fps

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    target = (args.host, args.port)
    print(f"sending {PIXELS_PER_LINE}x{height} @ {fps:g} fps to "
          f"{args.host}:{args.port}", file=sys.stderr)

    seq = 0
    frame_no = 0
    next_t = time.monotonic()
    try:
        while True:
            frame = build_test_frame(height, frame_no)
            n_packets = height // LINES_PER_PACKET
            for pkt_idx in range(n_packets):
                y = pkt_idx * LINES_PER_PACKET
                last = (pkt_idx == n_packets - 1)
                line_field = y | (0x8000 if last else 0)
                # Header: seq u16, frame u16, line u16, pxline u16,
                # lpp u8, bpp u8, encoding u16  (all little-endian).
                hdr = struct.pack("<HHHHBBH",
                                  seq & 0xFFFF,
                                  frame_no & 0xFFFF,
                                  line_field,
                                  PIXELS_PER_LINE,
                                  LINES_PER_PACKET,
                                  4,  # bpp
                                  0,  # encoding
                                  )
                payload = pack_lines(frame, y, LINES_PER_PACKET)
                sock.sendto(hdr + payload, target)
                seq = (seq + 1) & 0xFFFF
            frame_no = (frame_no + 1) & 0xFFFF

            next_t += frame_period
            slack = next_t - time.monotonic()
            if slack > 0:
                time.sleep(slack)
            else:
                # Falling behind: skip ahead, don't accumulate.
                next_t = time.monotonic()
    except KeyboardInterrupt:
        print("\nstopped", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
