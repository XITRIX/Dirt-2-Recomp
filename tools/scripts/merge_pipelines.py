#!/usr/bin/env python3
"""Merges DXVK pipeline record files (third_party/patches/dxvk/0003-pipeline-warmup.patch).

Each run of the game appends the pipelines it compiles to <data>/dirt2.pipelines
(DXVK_PIPELINE_RECORD). This merges such files from several play-throughs or
devices into one file to ship, dropping duplicates and torn entries. Files from
another DXVK build (version, state layout) are refused.

With --lut, also reports how many pipelines can be warmed up with that shader
cache (dirt2.dxvk.lut): a pipeline is skipped at warm-up when one of its
shaders is missing from the cache that ships with it.

Usage: merge_pipelines.py -o build/pipelines/dirt2.pipelines [--lut dirt2.dxvk.lut] in.pipelines...
"""
import argparse
import struct
import sys

HEADER = struct.Struct("<4sIII64s")  # magic, format, sizeof(options), sizeof(state), version
ENTRY = struct.Struct("<IQ")         # payload size, fnv1a-64(payload)


def fnv1a(data):
    """DXVK's bit::fnv1a_hash: FNV-1a over little-endian 64-bit words, then the length."""
    def step(h, v):
        return ((h ^ v) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    h = 0xCBF29CE484222325
    for i in range(0, len(data), 8):
        h = step(h, int.from_bytes(data[i:i + 8], "little"))
    return step(h, len(data))


def read_pipelines(path):
    data = open(path, "rb").read()
    if len(data) < HEADER.size or data[:4] != b"D2PL":
        sys.exit(f"{path}: not a pipeline file")
    header = data[:HEADER.size]
    entries, off = [], HEADER.size
    while off + ENTRY.size <= len(data):
        size, checksum = ENTRY.unpack_from(data, off)
        payload = data[off + ENTRY.size:off + ENTRY.size + size]
        if len(payload) != size or fnv1a(payload) != checksum:
            print(f"{path}: dropping a torn entry at byte {off}", file=sys.stderr)
            break
        entries.append((checksum, payload))
        off += ENTRY.size + size
    return header, entries


def shader_keys(payload, options_size):
    """Yields (name, create info bytes) for each shader of a pipeline."""
    mask, off = payload[0], 1
    for stage in range(5):
        if mask & (1 << stage):
            (n,) = struct.unpack_from("<H", payload, off)
            name = payload[off + 2:off + 2 + n]
            off += 2 + n
            info = payload[off:off + options_size + 8]
            off += options_size + 8
            yield name, info


def read_lut(path, options_size):
    """Shader keys in a DXVK shader cache look-up table."""
    data = open(path, "rb").read()
    (n,) = struct.unpack_from("<H", data, 4)
    off, keys = 6 + n, set()
    while off < len(data):
        (n,) = struct.unpack_from("<H", data, off)
        name = data[off + 2:off + 2 + n]
        off += 2 + n
        info = data[off:off + options_size + 8]
        off += options_size + 8
        (xfb,) = struct.unpack_from("<I", data, off)
        if xfb:
            sys.exit(f"{path}: shaders with transform feedback are not supported here")
        off += 4 + 24  # xfb count, LutEntry (offset, sizes, checksum)
        keys.add((name, info))
    return keys


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--lut", help="shader cache look-up table that ships with the output")
    ap.add_argument("inputs", nargs="+")
    args = ap.parse_args()

    header, seen, merged = None, set(), []
    for path in args.inputs:
        h, entries = read_pipelines(path)
        if header is None:
            header = h
        elif h != header:
            _, fmt, _, state, ver = HEADER.unpack(h)
            sys.exit(f"{path}: from another DXVK build ({ver.rstrip(b'\0').decode()}, format {fmt}, state {state} bytes)")
        new = [e for e in entries if e[0] not in seen]
        seen.update(e[0] for e in new)
        merged += new
        print(f"{path}: {len(entries)} pipelines, {len(new)} new")

    with open(args.output, "wb") as f:
        f.write(header)
        for checksum, payload in merged:
            f.write(ENTRY.pack(len(payload), checksum) + payload)
    _, _, options_size, _, version = HEADER.unpack(header)
    print(f"{args.output}: {len(merged)} pipelines (DXVK {version.rstrip(b'\0').decode()})")

    if args.lut:
        keys = read_lut(args.lut, options_size)
        covered = sum(all(k in keys for k in shader_keys(p, options_size)) for _, p in merged)
        print(f"{args.lut}: {len(keys)} shaders; {covered} of {len(merged)} pipelines can be warmed up")


if __name__ == "__main__":
    main()
