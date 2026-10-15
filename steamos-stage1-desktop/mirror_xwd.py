#!/usr/bin/env python3
"""mirror_xwd.py -- drive the presenter's frame file from the X :99 root window.

This is the single owner of the capture loop. It:

  1. dumps the X root window with `xwd` into a sibling temp file,
  2. optionally mirrors the left eye half of the image onto the right eye
     half, so both lenses show identical content and fuse (the panel shows
     the left half of the 3664x1920 desktop to the left eye and the right
     half to the right eye, so an unmirrored full-width desktop shows each
     eye something different and the user sees rivalry, not a desktop),
  3. atomically renames the temp file over the frame file.

Doing capture and mirror in one process is the point: the previous split
between an `xwd` loop and a separate mirror daemon meant the mirror could
rewrite the frame file while /usr/local/bin/kms_dual_image was reading it,
so the panel could tear. Renaming a fully written file is atomic, so the
presenter only ever sees complete frames.

The XWD header is parsed properly (header_size + ncolors * 12 for the data
offset, bytes_per_line for the row stride) rather than assuming a fixed
3179-byte prefix, which is what used to break on Xvfb's colormap.

Usage:
    mirror_xwd.py [--display :99] [--frame /run/frame.xwd]
                  [--period-ms 200] [--mirror 0|1] [--once]
"""

import argparse
import mmap
import os
import signal
import struct
import subprocess
import sys
import time

XWD_COLOR_SIZE = 12  # pixel(4) + r/g/b(2 each) + flags(1) + pad(1)


def parse_header(buf):
    """Return (data_offset, width, height, bytes_per_line, bytes_per_pixel)."""
    if len(buf) < 100:
        raise ValueError("short XWD header")
    header_size = struct.unpack_from(">I", buf, 0)[0]
    width, height = struct.unpack_from(">II", buf, 16)
    bits_per_pixel = struct.unpack_from(">I", buf, 44)[0]
    bytes_per_line = struct.unpack_from(">I", buf, 48)[0]
    ncolors = struct.unpack_from(">I", buf, 76)[0]

    offset = header_size + ncolors * XWD_COLOR_SIZE
    if bytes_per_line <= 0:
        bytes_per_line = width * (bits_per_pixel // 8)

    # Sanity: the pixel data has to fit. Xvfb writes ncolors=0 for TrueColor,
    # but other servers write a full colormap, so trust the file over the
    # guess when they disagree.
    if offset + bytes_per_line * height > len(buf) and \
            header_size + bytes_per_line * height <= len(buf):
        offset = header_size

    if bits_per_pixel % 8:
        raise ValueError(f"unsupported bits_per_pixel {bits_per_pixel}")
    return offset, width, height, bytes_per_line, bits_per_pixel // 8


def mirror_in_place(path, offset, width, height, stride, bpp_bytes):
    """Copy the left half of every row over the right half."""
    half = (width // 2) * bpp_bytes
    if half <= 0:
        return 0
    with open(path, "r+b") as f:
        with mmap.mmap(f.fileno(), 0) as mm:
            for y in range(height):
                row = offset + y * stride
                mm[row + half:row + 2 * half] = mm[row:row + half]
            mm.flush()
    return height


def capture_once(a, tmp):
    cmd = ["xwd", "-display", a.display, "-root", "-silent", "-out", tmp]
    try:
        subprocess.run(cmd, check=True, timeout=20,
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    except (subprocess.CalledProcessError, subprocess.TimeoutExpired,
            FileNotFoundError) as e:
        print(f"mirror_xwd: xwd failed: {e}", file=sys.stderr, flush=True)
        return False

    if a.mirror:
        try:
            with open(tmp, "rb") as f:
                head = f.read(4096)
            offset, w, h, stride, bpp = parse_header(head)
            mirror_in_place(tmp, offset, w, h, stride, bpp)
        except Exception as e:  # never let a bad frame stop the pump
            print(f"mirror_xwd: mirror skipped: {e}", file=sys.stderr,
                  flush=True)

    os.replace(tmp, a.frame)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--display", default=":99")
    ap.add_argument("--frame", default="/run/frame.xwd")
    ap.add_argument("--period-ms", type=int, default=200)
    ap.add_argument("--mirror", type=int, default=1)
    ap.add_argument("--once", action="store_true")
    a = ap.parse_args()

    stop = {"now": False}

    def on_signal(_sig, _frm):
        stop["now"] = True

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)

    tmp = a.frame + ".tmp"
    period = max(a.period_ms, 20) / 1000.0

    print(f"mirror_xwd: {a.display} -> {a.frame} every {a.period_ms}ms "
          f"(mirror={a.mirror})", flush=True)

    while not stop["now"]:
        started = time.monotonic()
        capture_once(a, tmp)
        if a.once:
            break
        elapsed = time.monotonic() - started
        if elapsed < period:
            time.sleep(period - elapsed)

    return 0


if __name__ == "__main__":
    sys.exit(main())
