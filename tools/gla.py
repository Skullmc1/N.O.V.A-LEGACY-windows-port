"""Read the game's .gla2 data packs.

Layout: 16-byte header, the files back to back (stored, not compressed), then an
index of 24-byte entries: name hash, offset and size (big-endian), then 12 bytes
we do not use. File names are stored only as hashes.

    python tools/gla.py list <pack> [how many entries to show]
    python tools/gla.py extract <pack> <outdir>
"""
import collections
import struct
import sys
from pathlib import Path

MAGICS = ((b"BRES", "bres"), (b"\xabKTX", "ktx"), (b"\x89PNG", "png"), (b"OggS", "ogg"), (b"RIFF", "riff"),
          (b"FWS", "swf"), (b"CWS", "swf"), (b"<", "xml"), (b"{", "json"), (b"[", "json"))


def entries(data):
    out = []
    pos = len(data) - 24
    while pos >= 16:
        h, off, size, zero = struct.unpack_from(">IIII", data, pos)
        if zero != 0 or off < 16 or off + size > pos:
            break
        out.append((h, off, size))
        pos -= 24
    out.reverse()
    return out


def kind(blob):
    """Guess a file's type from its first bytes (names are not stored)."""
    for magic, name in MAGICS:
        if blob.startswith(magic):
            return name
    text = blob[:256]
    if text and all(32 <= c < 127 or c in (9, 10, 13) for c in text):
        return "txt"
    return "bin"


def main():
    cmd, pack = sys.argv[1], Path(sys.argv[2])
    data = pack.read_bytes()
    ents = entries(data)
    if cmd == "list":
        kinds = collections.Counter(kind(data[off:off + 256]) for _, off, _ in ents)
        print(f"{pack.name}: {len(ents)} files {dict(kinds.most_common())}")
        for h, off, size in ents[:int(sys.argv[3]) if len(sys.argv) > 3 else 20]:
            blob = data[off:off + size]
            print(f"  {h:08x} {off:9d} {size:9d} {kind(blob):5} {blob[:40]!r}")
    elif cmd == "extract":
        out = Path(sys.argv[3])
        out.mkdir(parents=True, exist_ok=True)
        for h, off, size in ents:
            blob = data[off:off + size]
            (out / f"{h:08x}.{kind(blob)}").write_bytes(blob)
        print(f"extracted {len(ents)} files to {out}")


if __name__ == "__main__":
    main()
