"""Stand-ins for EGL, OpenGL ES 2, the native window, sensors and OpenSL ES.
Nothing is drawn: calls succeed with plausible values so the game keeps
going, and the interesting ones (shaders, texture formats, frames) are logged."""
import collections
import re
import struct
from pathlib import Path

from emu import DATA_INIT, FALLBACKS, imp

WIDTH, HEIGHT = 1280, 720
SHADER_DIR = Path(__file__).resolve().parent.parent / "work" / "shaders"
STATS = collections.Counter()
TEX_FORMATS = collections.Counter()

EXTENSIONS = ("GL_OES_depth24 GL_OES_packed_depth_stencil GL_OES_rgb8_rgba8 GL_OES_element_index_uint "
              "GL_OES_texture_npot GL_OES_depth_texture GL_OES_standard_derivatives "
              "GL_OES_compressed_ETC1_RGB8_texture GL_EXT_texture_format_BGRA8888 GL_OES_vertex_half_float")
STRINGS = {0x1F00: "Qualcomm", 0x1F01: "Adreno (TM) 540", 0x1F02: "OpenGL ES 2.0", 0x1F03: EXTENSIONS,
           0x8B8C: "OpenGL ES GLSL ES 1.00"}
INTS = {0x0D33: 4096, 0x8869: 16, 0x8872: 16, 0x8B4D: 32, 0x8B4C: 16, 0x8DFB: 256, 0x8DFD: 224, 0x8DFC: 16,
        0x84E8: 4096, 0x851C: 4096, 0x86A2: 0, 0x8CA6: 0, 0x0D50: 8, 0x0D56: 24, 0x0D57: 8}
CONFIG = {0x3020: 32, 0x3021: 8, 0x3022: 8, 0x3023: 8, 0x3024: 8, 0x3025: 24, 0x3026: 8, 0x302E: 1,
          0x3031: 0, 0x3032: 0, 0x3033: 5, 0x3040: 4, 0x3028: 1}
_cache = {}
_ids = [100]


def new_id():
    _ids[0] += 1
    return _ids[0]


def cached_str(e, s):
    if s not in _cache:
        _cache[s] = e.alloc_cstr(s)
    return _cache[s]


def once(key, msg):
    if not STATS[key]:
        print(msg)
    STATS[key] += 1


# ---- native window / looper / sensors -------------------------------------
@imp("ANativeWindow_fromSurface", "ALooper_forThread", "ALooper_prepare", "ASensorManager_getInstance",
     "ASensorManager_createEventQueue")
def _token(e):
    return 0x7001000


@imp("ANativeWindow_getWidth")
def _ww(e):
    return WIDTH


@imp("ANativeWindow_getHeight")
def _wh(e):
    return HEIGHT


@imp("ANativeWindow_setBuffersGeometry", "ANativeWindow_release", "ASensorManager_getDefaultSensor",
     "ASensorEventQueue_enableSensor", "ASensorEventQueue_disableSensor", "ASensorEventQueue_setEventRate",
     "ASensorEventQueue_getEvents")
def _zero(e):
    return 0


# ---- EGL ------------------------------------------------------------------
@imp("eglGetDisplay", "eglGetCurrentDisplay")
def _dpy(e):
    return 1


@imp("eglInitialize")
def _init(e):
    if e.x(1):
        e.w32(e.x(1), 1)
    if e.x(2):
        e.w32(e.x(2), 4)
    return 1


@imp("eglChooseConfig")
def _choose(e):
    if e.x(2) and e.sx(3) > 0:
        e.w64(e.x(2), 1)
    e.w32(e.x(4), 1)
    return 1


@imp("eglGetConfigAttrib")
def _cfg(e):
    e.w32(e.x(3), CONFIG.get(e.x(2) & 0xFFFF, 0))
    return 1


@imp("eglCreateWindowSurface", "eglCreatePbufferSurface")
def _surf(e):
    print("  [egl] surface created")
    return 0x51


@imp("eglCreateContext", "eglGetCurrentContext")
def _ctx(e):
    return 0xC1


@imp("eglMakeCurrent", "eglDestroyContext", "eglDestroySurface", "eglTerminate")
def _true(e):
    return 1


@imp("eglQuerySurface")
def _qsurf(e):
    e.w32(e.x(3), {0x3057: WIDTH, 0x3056: HEIGHT}.get(e.x(2) & 0xFFFF, 0))
    return 1


@imp("eglGetError")
def _eglerr(e):
    return 0x3000


@imp("eglQueryString")
def _eglstr(e):
    return cached_str(e, "")


@imp("eglGetProcAddress")
def _eglproc(e):
    return 0


@imp("eglSwapBuffers")
def _swap(e):
    STATS["frames"] += 1
    n = STATS["frames"]
    if n <= 5 or n % 50 == 0:
        print(f"  [egl] frame {n} presented ({STATS['draws']} draw calls so far)")
    return 1


# ---- OpenGL ES 2 ----------------------------------------------------------
FALLBACKS.append(("gl", lambda e: 0))       # state-setting calls need no answer
SHADERS = {}        # id -> [type, source]
PROGRAMS = {}       # id -> {"shaders": [], "uniforms": [], "attribs": []}
GLSL_TYPES = {"float": 0x1406, "vec2": 0x8B50, "vec3": 0x8B51, "vec4": 0x8B52, "int": 0x1404, "bool": 0x8B56,
              "mat2": 0x8B5A, "mat3": 0x8B5B, "mat4": 0x8B5C, "sampler2D": 0x8B5E, "samplerCube": 0x8B60,
              "ivec2": 0x8B53, "ivec3": 0x8B54, "ivec4": 0x8B55}
DECL = re.compile(r"\b(uniform|attribute)\s+(?:(?:lowp|mediump|highp)\s+)?(\w+)\s+(\w+)\s*(?:\[\s*(\w+)\s*\])?\s*;")


@imp("glGetString")
def _glstr(e):
    return cached_str(e, STRINGS.get(e.x(0) & 0xFFFF, ""))


@imp("glGetIntegerv")
def _geti(e):
    p = e.x(0) & 0xFFFF
    if p == 0x0BA2 or p == 0x0C10:
        e.write(e.x(1), struct.pack("<4i", 0, 0, WIDTH, HEIGHT))
        return
    if p not in INTS:
        once(("int", p), f"  [gl] glGetIntegerv({p:#06x}) -> 0   (default)")
    e.w32(e.x(1), INTS.get(p, 0))


@imp("glGetFloatv")
def _getf(e):
    e.write(e.x(1), struct.pack("<2f", 1.0, 16.0 if (e.x(0) & 0xFFFF) == 0x84FF else 1.0))


@imp("glGetError")
def _glerr(e):
    return 0


@imp("glCheckFramebufferStatus")
def _fbstatus(e):
    return 0x8CD5


@imp("glGenTextures", "glGenBuffers", "glGenFramebuffers", "glGenRenderbuffers")
def _gen(e):
    for i in range(e.sx(0)):
        e.w32(e.x(1) + 4 * i, new_id())


@imp("glCreateShader")
def _mkshader(e):
    sid = new_id()
    SHADERS[sid] = [e.x(0) & 0xFFFF, ""]
    return sid


@imp("glShaderSource")
def _source(e):
    sid, count, strs, lens = e.x(0) & 0xFFFFFFFF, e.sx(1), e.x(2), e.x(3)
    parts = []
    for i in range(count):
        p = e.u64(strs + 8 * i)
        n = struct.unpack("<i", e.read(lens + 4 * i, 4))[0] if lens else -1
        parts.append(e.read(p, n).decode("utf-8", "replace") if n >= 0 else e.cstr(p))
    src = "".join(parts)
    if sid in SHADERS:
        SHADERS[sid][1] = src
        SHADER_DIR.mkdir(parents=True, exist_ok=True)
        ext = "vert" if SHADERS[sid][0] == 0x8B31 else "frag"
        (SHADER_DIR / f"{sid}.{ext}").write_text(src, encoding="utf-8")
    STATS["shaders"] += 1


@imp("glGetShaderiv")
def _shaderiv(e):
    p = e.x(1) & 0xFFFF
    src = SHADERS.get(e.x(0) & 0xFFFFFFFF, [0, ""])[1]
    e.w32(e.x(2), {0x8B81: 1, 0x8B84: 0, 0x8B88: len(src) + 1, 0x8B4F: SHADERS.get(e.x(0), [0])[0]}.get(p, 0))


@imp("glCreateProgram")
def _mkprog(e):
    pid = new_id()
    PROGRAMS[pid] = {"shaders": [], "uniforms": [], "attribs": []}
    return pid


@imp("glAttachShader")
def _attach(e):
    prog = PROGRAMS.get(e.x(0) & 0xFFFFFFFF)
    if prog is not None:
        prog["shaders"].append(e.x(1) & 0xFFFFFFFF)


@imp("glLinkProgram")
def _link(e):
    prog = PROGRAMS.get(e.x(0) & 0xFFFFFFFF)
    if prog is None:
        return
    seen = set()
    prog["uniforms"], prog["attribs"] = [], []
    for sid in prog["shaders"]:
        for kind, typ, name, arr in DECL.findall(SHADERS.get(sid, [0, ""])[1]):
            if (kind, name) in seen or typ not in GLSL_TYPES:
                continue
            seen.add((kind, name))
            size = int(arr) if arr and arr.isdigit() else 1
            prog["uniforms" if kind == "uniform" else "attribs"].append((name, GLSL_TYPES[typ], size))
    STATS["programs"] += 1


@imp("glGetProgramiv")
def _progiv(e):
    prog = PROGRAMS.get(e.x(0) & 0xFFFFFFFF, {"uniforms": [], "attribs": []})
    p = e.x(1) & 0xFFFF
    longest = lambda items: max([len(n) + 4 for n, _, _ in items] or [1])
    e.w32(e.x(2), {0x8B82: 1, 0x8B83: 1, 0x8B84: 0, 0x8B86: len(prog["uniforms"]),
                   0x8B89: len(prog["attribs"]), 0x8B87: longest(prog["uniforms"]),
                   0x8B8A: longest(prog["attribs"])}.get(p, 0))


def active(e, key):
    prog = PROGRAMS.get(e.x(0) & 0xFFFFFFFF)
    idx = e.x(1) & 0xFFFFFFFF
    if prog is None or idx >= len(prog[key]):
        return
    name, typ, size = prog[key][idx]
    text = (name + ("[0]" if size > 1 else "")).encode()[:max(e.sx(2) - 1, 0)]
    if e.x(3):
        e.w32(e.x(3), len(text))
    e.w32(e.x(4), size)
    e.w32(e.x(5), typ)
    e.write(e.x(6), text + b"\0")


@imp("glGetActiveUniform")
def _actu(e):
    active(e, "uniforms")


@imp("glGetActiveAttrib")
def _acta(e):
    active(e, "attribs")


def location(e, key):
    prog = PROGRAMS.get(e.x(0) & 0xFFFFFFFF)
    name = e.cstr(e.x(1)).split("[")[0]
    if prog is not None:
        for i, (n, _, _) in enumerate(prog[key]):
            if n == name:
                return i
    return 0xFFFFFFFF


@imp("glGetUniformLocation")
def _uloc(e):
    return location(e, "uniforms")


@imp("glGetAttribLocation")
def _aloc(e):
    return location(e, "attribs")


@imp("glGetShaderInfoLog", "glGetProgramInfoLog")
def _infolog(e):
    if e.x(2):
        e.w32(e.x(2), 0)
    if e.x(3) and e.sx(1) > 0:
        e.write(e.x(3), b"\0")


@imp("glCompressedTexImage2D")
def _ctex(e):
    TEX_FORMATS[f"compressed {e.x(2) & 0xFFFF:#06x}"] += 1


@imp("glTexImage2D")
def _tex(e):
    TEX_FORMATS[f"plain fmt={e.x(6) & 0xFFFF:#06x} type={e.x(7) & 0xFFFF:#06x}"] += 1


@imp("glDrawArrays", "glDrawElements")
def _draw(e):
    STATS["draws"] += 1


@imp("glReadPixels")
def _readpixels(e):
    return 0


# ---- OpenSL ES --------------------------------------------------------------
# Objects and interfaces are pointers to a table of function pointers. Every
# slot succeeds; the few that hand back another object are special-cased.
def sl_interface(e, kind):
    table = e.malloc(8 * 32)
    for i in range(32):
        e.w64(table + 8 * i, e.stub(f"SL:{kind}#{i}", lambda em, k=kind, i=i: sl_call(em, k, i)))
    itf = e.malloc(8)
    e.w64(itf, table)
    return itf


def sl_call(e, kind, idx):
    once(("sl", kind, idx), f"  [audio] OpenSL {kind} method #{idx}")
    if kind == "object" and idx == 3:                 # GetInterface(self, iid, *out)
        tag = e.read(e.x(1), 1)[0] if e.x(1) else 0
        e.w64(e.x(2), sl_interface(e, {1: "engine", 2: "play", 3: "bufferqueue"}.get(tag, "other")))
    elif kind == "engine" and idx in (2, 7):          # CreateAudioPlayer / CreateOutputMix
        e.w64(e.x(1), sl_interface(e, "object"))
    return 0


@imp("slCreateEngine")
def _slcreate(e):
    print("  [audio] slCreateEngine")
    e.w64(e.x(0), sl_interface(e, "object"))
    return 0
