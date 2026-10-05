"""Android-specific pieces: the asset manager, wide-char helpers and Python
versions of the Java methods the native code calls."""
from pathlib import Path

from emu import imp
from jni import JAVA_METHODS
from libc import PKG, signed

ASSETS = Path(__file__).resolve().parent.parent / "work" / "apk" / "assets"
UTILS = f"com/gameloft/android/ANMP/GloftNOHM/PackageUtils/AndroidUtils"
OPEN = {}      # AAsset* -> [bytes, position, guest buffer]
MISSING = set()


@imp("AAssetManager_fromJava")
def _mgr(e):
    return e.stub_addr["AAssetManager_fromJava"]      # opaque non-null token


@imp("AAssetManager_open")
def _aopen(e):
    name = e.cstr(e.x(1))
    path = ASSETS / name.lstrip("./")
    ok = path.is_file()
    if ok or name not in MISSING:
        print(f"  [asset] open {name!r} -> {'ok, ' + format(path.stat().st_size, ',') + ' bytes' if ok else 'MISSING'}")
    MISSING.add(name)
    if not ok:
        return 0
    h = e.malloc(16)
    OPEN[h] = [path.read_bytes(), 0, 0]
    return h


@imp("AAsset_getLength")
def _alen(e):
    return len(OPEN[e.x(0)][0])


@imp("AAsset_read")
def _aread(e):
    a = OPEN[e.x(0)]
    data = a[0][a[1]:a[1] + e.x(2)]
    e.write(e.x(1), data)
    a[1] += len(data)
    return len(data)


@imp("AAsset_seek")
def _aseek(e):
    a = OPEN[e.x(0)]
    off, whence = signed(e.x(1), 64), e.sx(2)
    a[1] = max(0, min(len(a[0]), off + (0, a[1], len(a[0]))[whence]))
    return a[1]


@imp("AAsset_getBuffer")
def _abuf(e):
    a = OPEN[e.x(0)]
    if not a[2]:
        a[2] = e.alloc_bytes(a[0])
    return a[2]


@imp("AAsset_close")
def _aclose(e):
    OPEN.pop(e.x(0), None)


@imp("AAssetManager_openDir")
def _aopendir(e):
    print(f"  [asset] openDir {e.cstr(e.x(1))!r}")
    return 0


# ---- wide characters (ASCII only is enough for the C++ locale setup) ----
@imp("btowc")
def _btowc(e):
    c = e.sx(0)
    return c if 0 <= c < 128 else 0xFFFFFFFF


@imp("wctob")
def _wctob(e):
    c = e.x(0) & 0xFFFFFFFF
    return c if c < 128 else 0xFFFFFFFF


WCTYPES = ["alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print", "punct", "space", "upper",
           "xdigit"]


@imp("wctype")
def _wctype(e):
    name = e.cstr(e.x(0))
    return WCTYPES.index(name) + 1 if name in WCTYPES else 0


@imp("mbtowc")
def _mbtowc(e):
    if not e.x(1):
        return 0
    c = e.read(e.x(1), 1)[0]
    if e.x(0):
        e.w32(e.x(0), c)
    return int(c != 0)


# ---- Java methods ---------------------------------------------------------
def java(cls, name, value):
    JAVA_METHODS[(cls, name)] = value if callable(value) else (lambda jvm, *a: value)


for _name, _value in {
    "RetrieveSDCardPath": "/sdcard",
    "RetrieveObbPath": "NA",
    "RetrieveDataPath": f"/data/data/{PKG}/files",
    "RetrieveSavePath": f"/data/data/{PKG}/files",
    "RetrieveTempPath": f"/data/data/{PKG}/cache",
    "RetrieveNativeLibraryPath": f"/data/app/{PKG}/lib/arm64",
    "GetAndroidID": "9774d56d682e549c",
    "GetSerial": "unknown",
    "GetCPUSerial": "0000000000000000",
    "GetDeviceManufacturer": "Google",
    "GetCPUAbi": "arm64-v8a",
    "GetDeviceModel": "Pixel 2",
    "GetPhoneProduct": "walleye",
    "GetPhoneDevice": "walleye",
    "GetFirmware": "10",
    "GetMacAddress": "02:00:00:00:00:00",
    "GetDeviceIMEI": "",
    "GetHDIDFV": "6f1c2a34-5b6d-4e7f-8a9b-0c1d2e3f4a5b",
}.items():
    java(UTILS, _name, _value)


# ---- SharedPreferences, kept in a JSON file between runs --------------------
import json
import struct
import uuid

import libc
from jni import JObject

PREFS_FILE = libc.FS / "prefs.json"
PREFS = json.loads(PREFS_FILE.read_text()) if PREFS_FILE.exists() else {}


def jstr(jvm, handle):
    o = jvm.get(handle)
    return o.value if o is not None and isinstance(o.value, str) else ""


def pref_save(convert):
    def fn(jvm, key, group, value):
        PREFS.setdefault(jstr(jvm, group), {})[jstr(jvm, key)] = convert(jvm, value)
        PREFS_FILE.write_text(json.dumps(PREFS, indent=1))
    return fn


def pref_get(convert):
    def fn(jvm, key, group, default):
        return PREFS.get(jstr(jvm, group), {}).get(jstr(jvm, key), convert(jvm, default))
    return fn


def pref_remove(jvm, key, group):
    if PREFS.get(jstr(jvm, group), {}).pop(jstr(jvm, key), None) is not None:
        PREFS_FILE.write_text(json.dumps(PREFS, indent=1))


_int = lambda jvm, v: libc.signed(v, 32)
_long = lambda jvm, v: libc.signed(v, 64)
_bool = lambda jvm, v: int(bool(v & 0xFF))
for _kind, _conv in (("Int", _int), ("Long", _long), ("Bool", _bool), ("String", jstr)):
    java(UTILS, "SavePreference" + _kind, pref_save(_conv))
    java(UTILS, "GetPreference" + _kind, pref_get(_conv))
java(UTILS, "RemovePreference", pref_remove)


# ---- app identity -------------------------------------------------------------
def java_array_hash(data):
    """java.util.Arrays.hashCode(byte[]), which is what Signature.hashCode() returns."""
    h = 1
    for b in data:
        h = (31 * h + (b - 256 if b > 127 else b)) & 0xFFFFFFFF
    return h


def barrels(jvm):
    cert = (libc.ROOT / "work" / "cert.der").read_bytes()      # the APK's own signing certificate
    return JObject("[Int", bytearray(struct.pack("<4I", java_array_hash(cert), 0, 0, 0)))


java(UTILS, "retrieveBarrels", barrels)
java(UTILS, "GetApkPath", libc.APK_PATH)
java(UTILS, "GetLibSoPath", libc.LIB_PATH)
java(UTILS, "RetrieveNativeLibraryPath", libc.LIB_PATH.rsplit("/", 1)[0])
java(UTILS, "GetDeviceLanguage", "en")
java(UTILS, "GetCountry", "US")
java(UTILS, "GetDeviceSettingsCountryCode", "US")
java(UTILS, "GetSimIsoCountryCode", "us")
java(UTILS, "GetXDpi", 320.0)
java(UTILS, "GetYDpi", 320.0)
java("com/gameloft/android/ANMP/GloftNOHM/GLUtils/SUtils", "GenerateUUID", lambda jvm: str(uuid.uuid4()))
