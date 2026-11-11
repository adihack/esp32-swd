# SPDX-License-Identifier: MIT
"""Firmware images: Intel HEX / raw binary -> runs of whole erase pages for a target's memory map.

Images are handled as segments, [(address, bytes)], sorted and non-overlapping, so a megabyte costs slices, not a
Python object per byte. read_hex() still offers the old {address: byte} form; page_runs() takes either.
"""


def hex_segments(path):
    """Intel HEX -> [(address, bytes)], contiguous records merged. Record types 00 data, 01 EOF, 02/04 address
    extension; 03/05 ignored. A later record overwrites an earlier one at the same address."""
    recs, base = [], 0
    with open(path) as f:
        for n, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            if not line.startswith(":"):
                raise ValueError("%s:%d: not an Intel HEX record" % (path, n))
            rec = bytes.fromhex(line[1:])
            if len(rec) < 5 or len(rec) != rec[0] + 5 or sum(rec) & 0xFF:
                raise ValueError("%s:%d: bad length or checksum" % (path, n))
            addr, typ, data = (rec[1] << 8) | rec[2], rec[3], rec[4:-1]
            if typ == 0 and data:
                recs.append((base + addr, data))
            elif typ == 1:
                break
            elif typ == 2:
                base = int.from_bytes(data, "big") << 4
            elif typ == 4:
                base = int.from_bytes(data, "big") << 16
    return _merge(recs)


def _merge(recs):
    """[(addr, bytes)] in any order, possibly overlapping (later wins) -> sorted, merged, non-overlapping."""
    if not recs:
        return []
    ordered = sorted(range(len(recs)), key=lambda i: recs[i][0])
    if all(recs[ordered[k]][0] + len(recs[ordered[k]][1]) <= recs[ordered[k + 1]][0] for k in range(len(recs) - 1)):
        out = []   # the usual case: no overlaps, just join what touches
        for i in ordered:
            a, d = recs[i]
            if out and out[-1][0] + len(out[-1][1]) == a:
                out[-1][1].extend(d)
            else:
                out.append([a, bytearray(d)])
        return [(a, bytes(d)) for a, d in out]
    mem = {}   # overlapping records (rare): resolve byte by byte, in file order
    for a, d in recs:
        for i, b in enumerate(d):
            mem[a + i] = b
    return segments_of(mem)


def segments_of(mem):
    """{address: byte} -> [(address, bytes)]."""
    out, cur, nxt = [], None, None
    for a in sorted(mem):
        if a != nxt:
            cur = bytearray()
            out.append((a, cur))
        cur.append(mem[a])
        nxt = a + 1
    return [(a, bytes(d)) for a, d in out]


def read_hex(path):
    """Intel HEX -> {address: byte} (compatibility form; hex_segments() is the fast one)."""
    return {a + i: b for a, d in hex_segments(path) for i, b in enumerate(d)}


def load(path, regions, addr=None):
    """Segments of a .hex, or of a .bin placed at addr (default: start of the first flash region)."""
    if path.lower().endswith((".hex", ".ihex")):
        return hex_segments(path)
    with open(path, "rb") as f:
        data = f.read()
    if addr is None:
        addr = next(r["start"] for r in regions if r["kind"] == "flash")
    return [(addr, data)] if data else []


def size(segs):
    return sum(len(d) for _, d in segs)


def page_runs(mem, regions):
    """Segments (or {address: byte}) -> [(start, bytes)] covering whole erase pages, adjacent pages merged, gaps
    0xFF."""
    segs = segments_of(mem) if isinstance(mem, dict) else mem
    pages = {}
    for a, d in segs:
        end = a + len(d)
        while a < end:
            r = next((r for r in regions if r["start"] <= a < r["start"] + r["size"]), None)
            if r is None:
                raise ValueError("address 0x%08X is outside every memory region of the target" % a)
            if r["kind"] not in ("flash", "otp"):
                raise ValueError("address 0x%08X is in %s, not flash" % (a, r["kind"]))
            pg = r["sector"]
            p0 = a - (a - r["start"]) % pg
            pages[p0] = pg
            a = min(p0 + pg, end)
    runs, cur = [], None
    for start in sorted(pages):
        if cur and cur[0] + cur[1] == start:
            cur[1] += pages[start]
        else:
            cur = [start, pages[start]]
            runs.append(cur)
    out = []
    for start, sz in runs:
        buf = bytearray(b"\xff" * sz)
        for a, d in segs:
            lo, hi = max(a, start), min(a + len(d), start + sz)
            if lo < hi:
                buf[lo - start:hi - start] = d[lo - a:hi - a]
        out.append((start, bytes(buf)))
    return out
