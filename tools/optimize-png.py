"""Lossless PNG shrink: unfilter, try every filter strategy, zlib level 9, keep only critical chunks.
Usage: python tools/optimize-png.py file.png ... | file.ico (PNG entries inside the ico are optimized in place)."""
import sys, struct, zlib

def chunks(d):
    i = 8
    while i < len(d):
        n, = struct.unpack('>I', d[i:i + 4]); t = d[i + 4:i + 8]
        yield t, d[i + 8:i + 8 + n]
        i += 12 + n

def mk(t, b):
    return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)

def paeth(a, b, c):
    p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)

def opt(d):
    cs = list(chunks(d))
    ihdr = next(b for t, b in cs if t == b'IHDR')
    w, h, depth, ctype, _, _, inter = struct.unpack('>IIBBBBB', ihdr)
    if depth != 8 or inter or ctype not in (2, 6):
        return d
    bpp = 4 if ctype == 6 else 3
    raw = zlib.decompress(b''.join(b for t, b in cs if t == b'IDAT'))
    stride = w * bpp
    rows, prev, i = [], bytearray(stride), 0
    for y in range(h):
        f = raw[i]; line = bytearray(raw[i + 1:i + 1 + stride]); i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0; b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
            line[x] = (line[x] + (0, a, b, (a + b) // 2, paeth(a, b, c))[f]) & 255
        rows.append(bytes(line)); prev = line
    def filt(f, line, prev):
        out = bytearray(stride)
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0; b = prev[x]; c = prev[x - bpp] if x >= bpp else 0
            out[x] = (line[x] - (0, a, b, (a + b) // 2, paeth(a, b, c))[f]) & 255
        return bytes([f]) + bytes(out)
    best = None
    for strat in range(6):  # 0-4 fixed, 5 = per-row min-sum heuristic
        prev, data = bytes(stride), bytearray()
        for line in rows:
            if strat < 5:
                data += filt(strat, line, prev)
            else:
                cand = [filt(f, line, prev) for f in range(5)]
                data += min(cand, key=lambda r: sum(v if v < 128 else 256 - v for v in r[1:]))
            prev = line
        z = zlib.compress(bytes(data), 9)
        if best is None or len(z) < len(best): best = z
    out = d[:8] + mk(b'IHDR', ihdr) + mk(b'IDAT', best) + mk(b'IEND', b'')
    return out if len(out) < len(d) else d

for p in sys.argv[1:]:
    d = open(p, 'rb').read()
    if p.lower().endswith('.ico'):
        n, = struct.unpack('<H', d[4:6]); ents = []
        for k in range(n):
            e = list(struct.unpack('<BBBBHHII', d[6 + 16 * k:22 + 16 * k]))
            img = d[e[7]:e[7] + e[6]]
            if img[:4] == b'\x89PNG': img = opt(img)
            ents.append((e, img))
        off, hdr, body = 6 + 16 * n, d[:6], b''
        for e, img in ents:
            e[6], e[7] = len(img), off + len(body); hdr += struct.pack('<BBBBHHII', *e); body += img
        new = hdr + body
    else:
        new = opt(d)
    print(p, len(d), '->', len(new))
    open(p, 'wb').write(new)
