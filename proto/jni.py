"""A fake Java VM: enough of the JNI function table for libNOVA.so to start,
logging every Java class, method and field the native code asks for."""
import collections
import re
import struct

from libc import PKG, RegArgs, VaList

JNI_NAMES = {4: "GetVersion", 6: "FindClass", 10: "GetSuperclass", 13: "Throw", 14: "ThrowNew",
             15: "ExceptionOccurred", 16: "ExceptionDescribe", 17: "ExceptionClear", 18: "FatalError",
             19: "PushLocalFrame", 20: "PopLocalFrame", 21: "NewGlobalRef", 22: "DeleteGlobalRef",
             23: "DeleteLocalRef", 24: "IsSameObject", 25: "NewLocalRef", 26: "EnsureLocalCapacity",
             27: "AllocObject", 28: "NewObject", 29: "NewObjectV", 30: "NewObjectA", 31: "GetObjectClass",
             32: "IsInstanceOf", 33: "GetMethodID", 94: "GetFieldID", 113: "GetStaticMethodID",
             144: "GetStaticFieldID", 163: "NewString", 164: "GetStringLength", 165: "GetStringChars",
             166: "ReleaseStringChars", 167: "NewStringUTF", 168: "GetStringUTFLength",
             169: "GetStringUTFChars", 170: "ReleaseStringUTFChars", 171: "GetArrayLength",
             172: "NewObjectArray", 173: "GetObjectArrayElement", 174: "SetObjectArrayElement",
             215: "RegisterNatives", 216: "UnregisterNatives", 217: "MonitorEnter", 218: "MonitorExit",
             219: "GetJavaVM", 220: "GetStringRegion", 221: "GetStringUTFRegion",
             222: "GetPrimitiveArrayCritical", 223: "ReleasePrimitiveArrayCritical",
             226: "NewWeakGlobalRef", 227: "DeleteWeakGlobalRef", 228: "ExceptionCheck",
             229: "NewDirectByteBuffer", 230: "GetDirectBufferAddress", 231: "GetDirectBufferCapacity",
             232: "GetObjectRefType"}
TYPES = ["Object", "Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double", "Void"]
PRIMS = ["Boolean", "Byte", "Char", "Short", "Int", "Long", "Float", "Double"]
PRIM_SIZE = dict(zip(PRIMS, (1, 1, 2, 2, 4, 8, 4, 8)))
for _base, _fmt in ((34, "Call{}Method"), (64, "CallNonvirtual{}Method"), (114, "CallStatic{}Method")):
    for _i, _t in enumerate(TYPES):
        for _j, _suffix in enumerate(("", "V", "A")):
            JNI_NAMES[_base + _i * 3 + _j] = _fmt.format(_t) + _suffix
for _i, _t in enumerate(TYPES[:9]):
    JNI_NAMES[95 + _i] = f"Get{_t}Field"
    JNI_NAMES[104 + _i] = f"Set{_t}Field"
    JNI_NAMES[145 + _i] = f"GetStatic{_t}Field"
    JNI_NAMES[154 + _i] = f"SetStatic{_t}Field"
for _i, _t in enumerate(PRIMS):
    JNI_NAMES[175 + _i] = f"New{_t}Array"
    JNI_NAMES[183 + _i] = f"Get{_t}ArrayElements"
    JNI_NAMES[191 + _i] = f"Release{_t}ArrayElements"
    JNI_NAMES[199 + _i] = f"Get{_t}ArrayRegion"
    JNI_NAMES[207 + _i] = f"Set{_t}ArrayRegion"

SIG = re.compile(r"\[*(?:L[^;]+;|[ZBCSIJFDV])")

# Values for static fields the game reads (android.os.Build etc).
STATIC_FIELDS = {
    ("android/os/Build", "MODEL"): "Pixel 2", ("android/os/Build", "MANUFACTURER"): "Google",
    ("android/os/Build", "PRODUCT"): "walleye", ("android/os/Build", "DEVICE"): "walleye",
    ("android/os/Build", "BRAND"): "google", ("android/os/Build", "HARDWARE"): "qcom",
    ("android/os/Build", "CPU_ABI"): "arm64-v8a", ("android/os/Build$VERSION", "RELEASE"): "10",
    ("android/os/Build$VERSION", "SDK_INT"): 29,
}
# Python implementations of Java methods, keyed by (class, name). Filled in as discovered.
JAVA_METHODS = {}


class JObject:
    def __init__(self, cls, value=None):
        self.cls, self.value = cls, value

    def __repr__(self):
        return f"<{self.cls}{'' if self.value is None else ' ' + repr(self.value)[:60]}>"


class JVM:
    def __init__(self, e):
        self.e = e
        self.objs = {}
        self.next = 0x1000
        self.classes = {}
        self.seen = collections.OrderedDict()     # (kind, class, name, sig) -> count
        self.natives = {}
        self.pending = {}                         # array elements handed to the guest

        env_tab = e.malloc(8 * 240)
        for i in range(240):
            name = JNI_NAMES.get(i, f"jni#{i}")
            e.w64(env_tab + 8 * i, e.stub(f"JNI:{name}", self._handler(name)))
        self.env = e.malloc(8)
        e.w64(self.env, env_tab)

        vm_tab = e.malloc(8 * 8)
        vm_fns = {3: lambda em: 0, 4: self._get_env, 5: lambda em: 0, 6: self._get_env, 7: self._get_env}
        for i in range(8):
            e.w64(vm_tab + 8 * i, e.stub(f"JavaVM#{i}", vm_fns.get(i, lambda em: 0)))
        self.vm = e.malloc(8)
        e.w64(self.vm, vm_tab)

    # ---- handles -------------------------------------------------------
    def new(self, obj):
        self.next += 8
        self.objs[self.next] = obj
        return self.next

    def get(self, h):
        return self.objs.get(h)

    def string(self, s):
        return self.new(JObject("java/lang/String", s))

    def cls(self, name):
        if name not in self.classes:
            self.classes[name] = self.new(JObject("java/lang/Class", name))
        return self.classes[name]

    def note(self, *key):
        first = key not in self.seen
        self.seen[key] = self.seen.get(key, 0) + 1
        return first

    # ---- dispatch ------------------------------------------------------
    def _get_env(self, e):
        e.w64(e.x(1), self.env)
        return 0

    def _handler(self, name):
        m = re.fullmatch(r"Call(Static|Nonvirtual)?(\w+?)Method([VA]?)", name)
        if m:
            return lambda e: self._call(e, *m.groups())
        m = re.fullmatch(r"(Get|Set)(Static)?(\w+)Field", name)
        if m and m.group(3) in TYPES:
            return lambda e: self._field(e, *m.groups())
        m = re.fullmatch(r"(New|Get|Release|Set)(\w+?)Array(Elements|Region)?", name)
        if m and m.group(2) in PRIMS:
            return lambda e: self._array(e, *m.groups())
        fn = getattr(self, "j_" + name, None)
        if fn:
            return fn

        def missing(e):
            if self.note("jni", name, "", ""):
                print(f"  !! unimplemented JNI function {name}  (from {e.sym(e.lr)})")
            return 0
        return missing

    def _ret(self, e, typ, value):
        """Convert a Python value to a JNI return for a signature type."""
        if typ == "V":
            return None
        if typ in "FD":
            (e.sets if typ == "F" else e.setd)(0, float(value or 0))
            return None
        if typ[0] in "L[":
            if value is None:
                if typ == "Ljava/lang/String;":
                    value = ""
                elif typ[0] == "[":
                    value = JObject(typ, bytearray() if typ[1] in "ZB" else [])
                else:
                    value = JObject(typ[1:-1])
            if isinstance(value, str):
                return self.string(value)
            if isinstance(value, JObject):
                return self.new(value)
            return int(value)
        return int(value or 0)

    def _call(self, e, kind, rtype, variant):
        target, mid = e.x(1), e.x(3 if kind == "Nonvirtual" else 2)
        first = 4 if kind == "Nonvirtual" else 3
        meth = self.get(mid)
        if meth is None:
            print(f"  !! Call{kind or ''}{rtype}Method with unknown method id {mid:#x}")
            return 0
        cls, name, sig = meth.value
        ptypes = SIG.findall(sig[1:sig.index(")")])
        rsig = sig[sig.index(")") + 1:]
        if variant == "A":
            base = e.x(first)
            raw = [e.u64(base + 8 * i) for i in range(len(ptypes))]
            args = [struct.unpack("<d", struct.pack("<Q", v))[0] if t == "D" else
                    struct.unpack("<f", struct.pack("<I", v & 0xFFFFFFFF))[0] if t == "F" else v
                    for t, v in zip(ptypes, raw)]
        else:
            src = VaList(e, e.x(first)) if variant == "V" else RegArgs(e, first)
            args = [src.double() if t in "FD" else src.int() for t in ptypes]
        shown = []
        for t, v in zip(ptypes, args):
            o = self.get(v) if t[0] in "L[" else None
            shown.append(repr(o.value)[:50] if o is not None and o.value is not None else
                         (f"{v:#x}" if isinstance(v, int) else f"{v:g}"))
        impl = JAVA_METHODS.get((cls, name))
        result = impl(self, *args) if impl else None
        if self.note("method", cls, name, sig) or impl is None and self.seen[("method", cls, name, sig)] <= 1:
            tag = "" if impl else "   (default)"
            print(f"  [java] {cls}.{name}{sig}  args=({', '.join(shown)}){tag}")
        return self._ret(e, rsig, result)

    def _field(self, e, op, static, typ):
        fld = self.get(e.x(2))
        if fld is None:
            return 0
        cls, name, sig = fld.value
        if op == "Set":
            if self.note("field-set", cls, name, sig):
                print(f"  [java] set {cls}.{name} ({sig})")
            return None
        value = STATIC_FIELDS.get((cls, name))
        if self.note("field", cls, name, sig):
            print(f"  [java] get {cls}.{name} ({sig}) -> {value!r}{'' if (cls, name) in STATIC_FIELDS else '   (default)'}")
        return self._ret(e, sig, value)

    def _array(self, e, op, prim, what):
        size = PRIM_SIZE[prim]
        if op == "New":
            return self.new(JObject("[" + prim, bytearray(e.sx(1) * size)))
        arr = self.get(e.x(1))
        buf = arr.value if arr is not None and isinstance(arr.value, (bytes, bytearray)) else bytearray()
        if what == "Elements":
            if op == "Get":
                if e.x(2):
                    e.write(e.x(2), b"\0")
                p = e.alloc_bytes(bytes(buf) or b"\0")
                self.pending[p] = arr
                return p
            arr2 = self.pending.pop(e.x(2), None)
            if arr2 is not None and isinstance(arr2.value, bytearray) and e.sx(3) != 2:
                arr2.value[:] = e.read(e.x(2), len(arr2.value))
            return None
        start, n = e.sx(2) * size, e.sx(3) * size
        if op == "Get":
            e.write(e.x(4), bytes(buf[start:start + n]).ljust(n, b"\0"))
        elif isinstance(buf, bytearray):
            buf[start:start + n] = e.read(e.x(4), n)
        return None

    # ---- individual JNI functions -------------------------------------
    def j_GetVersion(self, e):
        return 0x10006

    def j_FindClass(self, e):
        name = e.cstr(e.x(1))
        if self.note("class", name, "", ""):
            print(f"  [java] FindClass {name}")
        return self.cls(name)

    def _member(self, e, kind):
        owner = self.get(e.x(1))
        cls = owner.value if owner is not None else "?"
        return self.new(JObject(kind, (cls, e.cstr(e.x(2)), e.cstr(e.x(3)))))

    def j_GetMethodID(self, e):
        return self._member(e, "methodID")

    def j_GetStaticMethodID(self, e):
        return self._member(e, "methodID")

    def j_GetFieldID(self, e):
        return self._member(e, "fieldID")

    def j_GetStaticFieldID(self, e):
        return self._member(e, "fieldID")

    def j_GetObjectClass(self, e):
        o = self.get(e.x(1))
        return self.cls(o.cls if o is not None else "java/lang/Object")

    def _new_object(self, e):
        c = self.get(e.x(1))
        name = c.value if c is not None else "?"
        if self.note("new", name, "", ""):
            print(f"  [java] new {name}")
        return self.new(JObject(name))

    j_NewObject = j_NewObjectV = j_NewObjectA = j_AllocObject = _new_object

    def _same(self, e):
        return e.x(1)

    j_NewGlobalRef = j_NewLocalRef = j_NewWeakGlobalRef = _same

    def _zero(self, e):
        return 0

    j_DeleteGlobalRef = j_DeleteLocalRef = j_DeleteWeakGlobalRef = _zero
    j_ExceptionCheck = j_ExceptionOccurred = j_ExceptionClear = j_ExceptionDescribe = _zero
    j_PushLocalFrame = j_EnsureLocalCapacity = j_MonitorEnter = j_MonitorExit = _zero
    j_ReleaseStringUTFChars = j_ReleaseStringChars = j_ReleasePrimitiveArrayCritical = _zero

    def j_PopLocalFrame(self, e):
        return e.x(1)

    def j_IsSameObject(self, e):
        return int(e.x(1) == e.x(2))

    def j_IsInstanceOf(self, e):
        return 1

    def j_NewStringUTF(self, e):
        return self.string(e.cstr(e.x(1)))

    def _str(self, e):
        o = self.get(e.x(1))
        return o.value if o is not None and isinstance(o.value, str) else ""

    def j_GetStringUTFChars(self, e):
        if e.x(2):
            e.write(e.x(2), b"\0")
        return e.alloc_cstr(self._str(e))

    def j_GetStringUTFLength(self, e):
        return len(self._str(e).encode())

    def j_GetStringLength(self, e):
        return len(self._str(e))

    def j_GetStringUTFRegion(self, e):
        s = self._str(e)[e.sx(2):e.sx(2) + e.sx(3)]
        e.write(e.x(4), s.encode() + b"\0")

    def j_GetArrayLength(self, e):
        o = self.get(e.x(1))
        if o is None or o.value is None:
            return 0
        size = PRIM_SIZE.get(o.cls[1:], 1) if isinstance(o.value, (bytes, bytearray)) else 1
        return len(o.value) // size

    def j_NewObjectArray(self, e):
        return self.new(JObject("[Ljava/lang/Object;", [0] * e.sx(1)))

    def j_GetObjectArrayElement(self, e):
        o = self.get(e.x(1))
        return o.value[e.sx(2)] if o is not None and isinstance(o.value, list) and e.sx(2) < len(o.value) else 0

    def j_SetObjectArrayElement(self, e):
        o = self.get(e.x(1))
        if o is not None and isinstance(o.value, list) and e.sx(2) < len(o.value):
            o.value[e.sx(2)] = e.x(3)

    def j_GetJavaVM(self, e):
        e.w64(e.x(1), self.vm)
        return 0

    def j_RegisterNatives(self, e):
        c = self.get(e.x(1))
        cls = c.value if c is not None else "?"
        for i in range(e.sx(3)):
            name, sig, fn = (e.u64(e.x(2) + 24 * i + 8 * k) for k in range(3))
            self.natives[(cls, e.cstr(name))] = fn
            print(f"  [java] RegisterNatives {cls}.{e.cstr(name)}{e.cstr(sig)} -> {e.sym(fn)}")
        return 0
