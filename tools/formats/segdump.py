#!/usr/bin/env python3
"""Walk a .seg file (frame stream: camera, paletted color, depth, events) and
optionally write frames as PNG. Output goes wherever --out says; never commit it.

Frame = u32 flags
  &1  camera: pos float[3], quat float[4]
  &2  color: 768-byte palette (6-bit RGB) + four 8-bit index tiles
      (256x256, 256x256, 256x128, 256x128) = 512x384
  &4  depth: &0x10 ? 0xC71 u32 "tiny" depth : 512x384 u8 (0x30000)
  &8  events: s32 count, then count events (size not walked here)
"""
import struct, sys, zlib, argparse, os

def png(path, w, h, rgb):
    raw = b''.join(b'\0' + rgb[y*w*3:(y+1)*w*3] for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))

def frame_rgb(col):
    pal = col[:768]
    tiles = [(0, 0, 256, 256, 0x300), (256, 0, 256, 256, 0x10300), (0, 256, 256, 128, 0x20300), (256, 256, 256, 128, 0x28300)]
    out = bytearray(512 * 384 * 3)
    lut = [bytes((min(255, pal[i*3] * 4), min(255, pal[i*3+1] * 4), min(255, pal[i*3+2] * 4))) for i in range(256)]
    for x0, y0, w, h, off in tiles:
        for y in range(h):
            row = col[off + y*w: off + (y+1)*w]
            o = ((y0 + y) * 512 + x0) * 3
            out[o:o + w*3] = b''.join(lut[b] for b in row)
    return bytes(out)

ap = argparse.ArgumentParser()
ap.add_argument('seg'); ap.add_argument('--out'); ap.add_argument('--every', type=int, default=0)
ap.add_argument('--max', type=int, default=10**9)
a = ap.parse_args()
f = open(a.seg, 'rb'); size = os.path.getsize(a.seg)
n = 0; stats = {}
while n < a.max:
    hdr = f.read(4)
    if len(hdr) < 4: break
    flags, = struct.unpack('<I', hdr)
    stats[flags] = stats.get(flags, 0) + 1
    if flags & 1: f.read(28)
    col = None
    if flags & 2: col = f.read(0x30300)
    if flags & 4: f.read(0xC71 * 4 if flags & 0x10 else 0x30000)
    if flags & 8:
        cnt, = struct.unpack('<i', f.read(4))
        if cnt:
            print('frame %d has %d events at %d: stopping walk (event size unknown here)' % (n, cnt, f.tell())); break
    if a.out and col and a.every and n % a.every == 0:
        png(os.path.join(a.out, '%s_%05d.png' % (os.path.basename(a.seg)[:-4], n)), 512, 384, frame_rgb(col))
    n += 1
print('frames %d, at %d of %d bytes, flags histogram %s' % (n, f.tell(), size, {hex(k): v for k, v in sorted(stats.items())}))
