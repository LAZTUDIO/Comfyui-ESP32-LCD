#!/usr/bin/env python3
"""Convert the panel framebuffer dump (/shot.raw, 480x320 RGB565 LE) to PNG.

Usage: python3 tools/raw_to_png.py /tmp/shot.raw /tmp/shot.png
"""
import struct
import sys
import zlib

W, H = 480, 320


def main() -> None:
    src = sys.argv[1] if len(sys.argv) > 1 else "/tmp/shot.raw"
    dst = sys.argv[2] if len(sys.argv) > 2 else "/tmp/shot.png"
    data = open(src, "rb").read()
    if len(data) < W * H * 2:
        raise SystemExit(f"{src}: expected {W*H*2} bytes, got {len(data)}")
    px = struct.unpack("<%dH" % (W * H), data[: W * H * 2])
    rows = bytearray()
    for y in range(H):
        rows.append(0)  # filter: none
        for x in range(W):
            v = px[y * W + x]
            rows += bytes((((v >> 11) & 0x1F) * 255 // 31,
                           ((v >> 5) & 0x3F) * 255 // 63,
                           (v & 0x1F) * 255 // 31))

    def chunk(tag: bytes, payload: bytes) -> bytes:
        body = tag + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    png = (b"\x89PNG\r\n\x1a\n"
           + chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
           + chunk(b"IEND", b""))
    open(dst, "wb").write(png)
    print(f"wrote {dst}")


if __name__ == "__main__":
    main()
