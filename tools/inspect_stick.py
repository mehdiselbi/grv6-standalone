#!/usr/bin/env python3
"""Phase 0d: report which rekordbox library format(s) a USB stick carries.

Usage (Mac, stick mounted):  python3 -I inspect_stick.py /Volumes/STICKNAME > stick_report.txt

Classification rules and where they come from:
  - Device Library (legacy):  PIONEER/rekordbox/export.pdb (+ exportExt.pdb if present)
    Source: Deep Symmetry export analysis, crate-digger, rekordcrate docs.
  - OneLibrary (formerly Device Library Plus): a file named exportLibrary.db,
    SQLCipher-encrypted SQLite. Source: pyrekordbox docs, rbox docs.
    Location on the stick is NOT assumed: the whole PIONEER tree is searched.
  - Analysis files: PIONEER/USBANLZ/**/ANLZ*.DAT|.EXT|.2EX. Source: rekordcrate docs.
Only stdlib. Read-only. Never writes to the stick.
"""
import os
import sys

PLAIN_SQLITE_MAGIC = b"SQLite format 3\x00"  # public SQLite file-format spec


def head(path, n=16):
    with open(path, "rb") as f:
        return f.read(n)


def main(root):
    pio = None
    for name in os.listdir(root):
        if name.upper() == "PIONEER":
            pio = os.path.join(root, name)
    if not pio:
        print("No PIONEER folder at stick root.")
        return 1

    pdb, dbs, anlz, other = [], [], {}, []
    for dirpath, _, files in os.walk(pio):
        for fn in files:
            p = os.path.join(dirpath, fn)
            rel = os.path.relpath(p, root)
            low = fn.lower()
            if low.endswith(".pdb"):
                pdb.append(rel)
            elif low.endswith(".db"):
                dbs.append(rel)
            elif fn.upper().startswith("ANLZ"):
                ext = os.path.splitext(fn)[1].upper()
                anlz[ext] = anlz.get(ext, 0) + 1
            else:
                other.append(rel)

    print("== Databases ==")
    for rel in pdb + dbs:
        p = os.path.join(root, rel)
        h = head(p)
        kind = "?"
        if rel.lower().endswith("export.pdb"):
            kind = "Device Library (legacy DeviceSQL .pdb)"
        elif rel.lower().endswith("exportext.pdb"):
            kind = "Device Library extension .pdb"
        elif os.path.basename(rel) == "exportLibrary.db":
            kind = ("OneLibrary, but header is PLAIN SQLite (unexpected, report this)"
                    if h == PLAIN_SQLITE_MAGIC else "OneLibrary (header not plain SQLite, consistent with SQLCipher)")
        print(f"{rel}\n   size={os.path.getsize(p)}  head={h.hex(' ')}\n   -> {kind}")

    print("\n== Analysis files by extension ==")
    for ext, n in sorted(anlz.items()):
        print(f"{ext}: {n}")

    print("\n== Other files under PIONEER (first 40) ==")
    for rel in sorted(other)[:40]:
        print(f"{rel}  {os.path.getsize(os.path.join(root, rel))} B")

    has_legacy = any(r.lower().endswith("export.pdb") for r in pdb)
    has_one = any(os.path.basename(r) == "exportLibrary.db" for r in dbs)
    print("\n== Verdict ==")
    print(f"Device Library: {'YES' if has_legacy else 'no'}   OneLibrary: {'YES' if has_one else 'no'}")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
