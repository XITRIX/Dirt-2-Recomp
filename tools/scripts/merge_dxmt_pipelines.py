#!/usr/bin/env python3
"""Merges DX11-path (DXMT) warm-up data from several runs or devices into the
set shipped with the app (dxmt/0004-pipeline-warmup.patch, HANDOVER.md):

  <out>/dirt2.dxmt-pipelines   recorded pipelines (DXPW records), duplicates dropped
  <out>/dxmt/shaders_<v>.db    DXMT's converted-shader cache, union of all inputs

Inputs are data folders (a device's Library/Application Support/Dirt2Recomp
copy, or a Mac DIRT2_DATA_DIR) holding dirt2.dxmt-pipelines and/or
dxmt/shaders_*.db (+ -wal, which is checkpointed into a working copy first).
Files from another DXMT build (record size or cache table differing) are
reported and skipped.

Usage: merge_dxmt_pipelines.py -o build/pipelines <data dir> [<data dir> ...]
"""
import argparse
import glob
import os
import shutil
import sqlite3
import struct
import sys
import tempfile

HEADER = struct.Struct("<4sIII")


def merge_pipelines(inputs, out):
    records, size, seen, total = [], None, set(), 0
    for d in inputs:
        path = os.path.join(d, "dirt2.dxmt-pipelines")
        if not os.path.exists(path):
            continue
        data = open(path, "rb").read()
        if len(data) < HEADER.size:
            continue
        magic, version, rsize, _ = HEADER.unpack_from(data)
        if magic != b"DXPW" or (size is not None and (version, rsize) != size):
            print(f"  {path}: other format ({magic!r} v{version}, {rsize}-byte records), skipped")
            continue
        size = (version, rsize)
        n = (len(data) - HEADER.size) // rsize  # a torn last record is dropped
        for i in range(n):
            rec = data[HEADER.size + i * rsize:HEADER.size + (i + 1) * rsize]
            total += 1
            if rec not in seen:
                seen.add(rec)
                records.append(rec)
        print(f"  {path}: {n} pipelines")
    if size is None:
        return 0
    with open(os.path.join(out, "dirt2.dxmt-pipelines"), "wb") as f:
        f.write(HEADER.pack(b"DXPW", size[0], size[1], 0))
        f.writelines(records)
    print(f"pipelines: {len(records)} unique of {total}")
    return len(records)


def merge_caches(inputs, out):
    os.makedirs(os.path.join(out, "dxmt"), exist_ok=True)
    by_name = {}
    for d in inputs:
        for db in glob.glob(os.path.join(d, "dxmt", "shaders_*.db")):
            by_name.setdefault(os.path.basename(db), []).append(db)
    for name, dbs in by_name.items():
        dst = os.path.join(out, "dxmt", name)
        if os.path.exists(dst):
            os.remove(dst)
        con = sqlite3.connect(dst)
        tables = set()
        with tempfile.TemporaryDirectory() as tmp:
            for i, db in enumerate(dbs):
                work = os.path.join(tmp, f"{i}.db")
                shutil.copy(db, work)
                if os.path.exists(db + "-wal"):
                    shutil.copy(db + "-wal", work + "-wal")
                src = sqlite3.connect(work)
                src.execute("PRAGMA wal_checkpoint(TRUNCATE)")
                src.close()
                con.execute(f"ATTACH DATABASE ? AS src{i}", (work,))
                for (table,) in con.execute(f"SELECT name FROM src{i}.sqlite_master WHERE type='table'"):
                    if table not in tables:
                        con.execute(f"CREATE TABLE IF NOT EXISTS {table} (key BLOB PRIMARY KEY, value BLOB NOT NULL)")
                        tables.add(table)
                    before = con.execute(f"SELECT count(*) FROM {table}").fetchone()[0]
                    con.execute(f"INSERT OR IGNORE INTO {table} SELECT key, value FROM src{i}.{table}")
                    after = con.execute(f"SELECT count(*) FROM {table}").fetchone()[0]
                    print(f"  {db}: +{after - before} shaders ({table})")
                con.commit()
                con.execute(f"DETACH DATABASE src{i}")
        con.execute("VACUUM")
        con.close()
        print(f"cache {name}: {os.path.getsize(dst) // 1024} KB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("inputs", nargs="+")
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    merge_pipelines(a.inputs, a.out)
    merge_caches(a.inputs, a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
