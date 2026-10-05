"""Discovery run: boot libNOVA.so under emulation the way MainActivity does
(load, NativeInit, NativeOnResume, NativeSurfaceChanged), then let its threads
run for a while and report what it asked the system, Java and the GPU for.

    python proto/run.py [--seconds N] [--trace]
"""
import argparse
import time
from pathlib import Path

import libc  # noqa: F401  (these imports register the import handlers)
import libc_extra  # noqa: F401
import android  # noqa: F401
import gfx
import sched
from emu import Emu
from jni import JObject, JVM

ROOT = Path(__file__).resolve().parent.parent
SO = ROOT / "work" / "lib" / "arm64-v8a" / "libNOVA.so"
BRIDGE = "com/gameloft/android/ANMP/GloftNOHM/PackageUtils/JNIBridge"
JNI_PREFIX = "Java_" + BRIDGE.replace("/", "_") + "_"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", action="store_true", help="print every import call")
    ap.add_argument("--seconds", type=int, default=180, help="how long to let the game run after startup")
    args = ap.parse_args()

    t = time.time()
    e = Emu(SO)
    vm = JVM(e)
    s = sched.Scheduler(e)
    ui = s.new_thread("java-ui")
    bridge = vm.cls(BRIDGE)
    print(f"loaded: {len(e.exports):,} exports, {len(e.stub_names)} stubs, "
          f"{len(e.init_array)} constructors ({time.time() - t:.1f}s)")
    e.trace = args.trace

    def stage(title, addr, *a, wall=300):
        print(f"\n=== {title}")
        t0 = time.time()
        r = s.call(ui, addr, *a, wall=wall)
        print(f"=== {title}: {'DID NOT FINISH' if r is None else f'returned {r:#x}'} ({time.time() - t0:.1f}s)")
        return r is not None

    def native(name, *a):
        return stage(name, e.exports[JNI_PREFIX + name], vm.env, bridge, *a)

    print("\n=== static constructors")
    t0 = time.time()
    ok = all(s.call(ui, fn, wall=120) is not None for fn in e.init_array if fn)
    print(f"=== static constructors: {'done' if ok else 'stopped'} ({time.time() - t0:.1f}s)")

    ok = ok and stage("JNI_OnLoad", e.exports["JNI_OnLoad"], vm.vm, 0)
    ok = ok and native("NativeInit")
    if ok:
        print("\n=== letting the game's main thread reach its event loop (10s)")
        s.run(wall=10)
        ok = not s.fault
    ok = ok and native("NativeOnResume")
    surface = vm.new(JObject("android/view/Surface"))
    ok = ok and native("NativeSurfaceChanged", surface, gfx.WIDTH, gfx.HEIGHT)

    def tick():
        print(f"  .. {time.time() - t0:.0f}s: frames={gfx.STATS['frames']} draws={gfx.STATS['draws']} "
              f"shaders={gfx.STATS['shaders']} heap={(e.heap_top - 0x80000000) / 1e6:.0f}MB "
              f"imports={sum(e.calls.values()):,}")

    if ok:
        print(f"\n=== running game threads for {args.seconds}s")
        t0 = time.time()
        s.run(wall=args.seconds, tick=tick)

    print("\n=== summary")
    s.status()
    print("import calls (top 20):", ", ".join(f"{n} x{c}" for n, c in e.calls.most_common(20)))
    if e.unimplemented:
        print("unimplemented imports hit:", ", ".join(f"{n} x{c}" for n, c in e.unimplemented.most_common()))
    print("graphics:", dict(gfx.STATS and {k: v for k, v in gfx.STATS.items() if isinstance(k, str)}))
    if gfx.TEX_FORMATS:
        print("texture uploads:", dict(gfx.TEX_FORMATS))
    print(f"guest heap high-water mark: {(e.heap_top - 0x80000000) / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
