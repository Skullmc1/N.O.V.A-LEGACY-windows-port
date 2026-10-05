"""Unpack the N.O.V.A. Legacy APK into work/.

The APK's lib/ folder only holds a small unpacker (libDecRawso). The real
native libraries live in assets/datv7 and assets/datv8: 7z archives whose
signature is replaced with "CORIAJ" and whose every byte is XORed with 0x61.
"""
import hashlib
import subprocess
import sys
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "work"
XOR_KEY = 0x61
# The one APK the port is known to work with: N.O.V.A. Legacy 5.8.4a.
APK_SHA256 = "ef570d2a31b0d167ae78d1a94bb4da847bb505342b3bb3b10865978360d90803"
SEVENZ_MAGIC = b"7z\xbc\xaf\x27\x1c"
# Windows ships bsdtar, which reads 7z archives.
TAR = r"C:\Windows\System32\tar.exe"


def decode_dat(src: Path, dst: Path) -> None:
    data = bytes(b ^ XOR_KEY for b in src.read_bytes())
    if data[:6] != b"CORIAJ":
        raise SystemExit(f"{src.name}: unexpected header {data[:6]!r}")
    dst.write_bytes(SEVENZ_MAGIC + data[6:])


def der_item(data: bytes, pos: int) -> tuple[int, int]:
    """Return (start, end) of the contents of the DER item at pos."""
    length = data[pos + 1]
    start = pos + 2
    if length & 0x80:
        count = length & 0x7F
        length = int.from_bytes(data[start:start + count], "big")
        start += count
    return start, start + length


def signing_cert(pkcs7: bytes) -> bytes:
    """Return the first certificate of a PKCS#7 signature block (META-INF/*.RSA)."""
    pos, _ = der_item(pkcs7, 0)       # ContentInfo
    _, pos = der_item(pkcs7, pos)     # signedData OID
    pos, _ = der_item(pkcs7, pos)     # [0]
    pos, _ = der_item(pkcs7, pos)     # SignedData
    while pkcs7[pos] != 0xA0:         # version, digest algorithms, content info
        _, pos = der_item(pkcs7, pos)
    pos, _ = der_item(pkcs7, pos)     # certificates
    _, end = der_item(pkcs7, pos)
    return pkcs7[pos:end]


def main() -> None:
    apks = sorted(ROOT.glob("*.apk"))
    if len(apks) != 1:
        raise SystemExit(f"expected exactly one .apk in {ROOT}, found {len(apks)}")
    digest = hashlib.sha256(apks[0].read_bytes()).hexdigest()
    if digest != APK_SHA256 and "--force" not in sys.argv:
        raise SystemExit(
            f"{apks[0].name} is not the APK this port was made for (N.O.V.A. Legacy 5.8.4a).\n"
            f"  expected SHA-256 {APK_SHA256}\n"
            f"  found    SHA-256 {digest}\n"
            "Other versions will most likely not run. Pass --force to unpack it anyway."
        )
    apk_dir = WORK / "apk"
    with zipfile.ZipFile(apks[0]) as z:
        z.extractall(apk_dir)
    print(f"extracted {apks[0].name} -> {apk_dir}")

    # The game checks the hash of its own signing certificate.
    rsa = next((apk_dir / "META-INF").glob("*.RSA"))
    (WORK / "cert.der").write_bytes(signing_cert(rsa.read_bytes()))

    lib_dir = WORK / "lib"
    lib_dir.mkdir(parents=True, exist_ok=True)
    for name in ("datv8", "datv7"):
        archive = WORK / f"{name}.7z"
        decode_dat(apk_dir / "assets" / name, archive)
        subprocess.run([TAR, "-xf", str(archive), "-C", str(lib_dir)], check=True)
        archive.unlink()
    for so in sorted(lib_dir.rglob("*.so")):
        print(f"  {so.relative_to(ROOT)}  {so.stat().st_size:,} bytes")


if __name__ == "__main__":
    sys.exit(main())
