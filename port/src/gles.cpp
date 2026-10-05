// EGL and OpenGL ES are forwarded to ANGLE (libEGL.dll / libGLESv2.dll),
// which renders through Direct3D 11. Guest pointers are host pointers, so
// almost every call passes straight through.
#include <windows.h>

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <EGL/egl.h>
#define GL_GLEXT_PROTOTYPES
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>

#include "guest.h"
#include "platform.h"

double gfx::shot_every = 0;
bool gfx::frame_stats = false;

namespace {
HMODULE g_egl = nullptr, g_gles = nullptr;
std::atomic<u64> g_frames{0};

template <class F>
F host_fn(HMODULE lib, const char* name) {
    auto p = reinterpret_cast<F>(GetProcAddress(lib, name));
    if (!p) fatal("ANGLE is missing %s", name);
    return p;
}

// Functions the game looks up by name at run time (the ES3 path): hand back a
// guest-callable stub that forwards to ANGLE.
using HandlerMaker = Handler (*)();
#define X(fn) {#fn, []() -> Handler { auto p = reinterpret_cast<decltype(&fn)>(GetProcAddress(g_gles, #fn)); return p ? wrap(p) : Handler(); }},
const std::unordered_map<std::string, HandlerMaker> g_makers = {
#include "gl_functions.inc"
};
#undef X

u64 lookup_by_name(const char* name) {
    static std::mutex m;
    static std::unordered_map<std::string, u64> stubs;
    std::lock_guard lk(m);
    auto hit = stubs.find(name);
    if (hit != stubs.end()) return hit->second;
    u64 addr = 0;
    if (Handler ours = guest::find_import(name)) {       // prefer our own wrapper when we have one
        addr = guest::make_stub(name, ours);
    } else if (auto mk = g_makers.find(name); mk != g_makers.end()) {
        Handler h = mk->second();
        if (h) addr = guest::make_stub(name, h);
    }
    if (!addr) logf("[egl] eglGetProcAddress(%s) -> not available", name);
    stubs[name] = addr;
    return addr;
}

// ---- in-game screenshots (--shot-every N): read the back buffer before it is presented
void write_png(const std::string& path, const std::vector<u8>& rgba, int w, int h) {
    auto crc32 = [](const u8* d, size_t n) {
        u32 c = ~0u;
        for (size_t i = 0; i < n; i++) {
            c ^= d[i];
            for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        }
        return ~c;
    };
    std::vector<u8> raw;                       // filter byte + RGB per row, flipped (GL rows are bottom-up)
    raw.reserve(static_cast<size_t>(h) * (1 + 3 * w));
    for (int y = h - 1; y >= 0; y--) {
        raw.push_back(0);
        for (int x = 0; x < w; x++) {
            const u8* p = &rgba[(static_cast<size_t>(y) * w + x) * 4];
            raw.insert(raw.end(), p, p + 3);
        }
    }
    std::vector<u8> z = {0x78, 0x01};          // zlib stream of stored (uncompressed) blocks
    u32 a = 1, b = 0;
    for (u8 v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    for (size_t off = 0; off < raw.size(); off += 65535) {
        u16 n = static_cast<u16>(std::min<size_t>(65535, raw.size() - off));
        z.push_back(off + n >= raw.size());
        z.push_back(n & 0xFF); z.push_back(n >> 8);
        z.push_back(~n & 0xFF); z.push_back((~n >> 8) & 0xFF);
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
    }
    u32 adler = (b << 16) | a;
    for (int i = 3; i >= 0; i--) z.push_back((adler >> (8 * i)) & 0xFF);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    auto be = [](u32 v) { return std::array<u8, 4>{u8(v >> 24), u8(v >> 16), u8(v >> 8), u8(v)}; };
    auto chunk = [&](const char* type, const std::vector<u8>& data) {
        std::vector<u8> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        fwrite(be(static_cast<u32>(data.size())).data(), 1, 4, f);
        fwrite(body.data(), 1, body.size(), f);
        fwrite(be(crc32(body.data(), body.size())).data(), 1, 4, f);
    };
    const u8 magic[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(magic, 1, 8, f);
    std::vector<u8> ihdr;
    for (u32 v : {static_cast<u32>(w), static_cast<u32>(h)}) {
        auto q = be(v);
        ihdr.insert(ihdr.end(), q.begin(), q.end());
    }
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    fclose(f);
}

void maybe_screenshot() {
    static auto last = std::chrono::steady_clock::now();
    static int index = 0;
    if (gfx::shot_every <= 0) return;
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration<double>(now - last).count() < gfx::shot_every) return;
    last = now;
    static auto get_int = host_fn<decltype(&glGetIntegerv)>(g_gles, "glGetIntegerv");
    static auto bind_fb = host_fn<decltype(&glBindFramebuffer)>(g_gles, "glBindFramebuffer");
    static auto read_px = host_fn<decltype(&glReadPixels)>(g_gles, "glReadPixels");
    GLint fb = 0;
    get_int(GL_FRAMEBUFFER_BINDING, &fb);
    bind_fb(GL_FRAMEBUFFER, 0);
    int w = platform::width(), h = platform::height();
    std::vector<u8> px(static_cast<size_t>(w) * h * 4);
    read_px(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    bind_fb(GL_FRAMEBUFFER, fb);
    char name[64];
    snprintf(name, sizeof name, "/work/shots/shot_%03d.png", index++);
    write_png(project_root() + name, px, w, h);
    logf("[shot] saved %s (frame %llu)", name, static_cast<unsigned long long>(g_frames.load()));
}
}  // namespace

#define EGL_FWD(name) guest::reg(#name, wrap(host_fn<decltype(&name)>(g_egl, #name)))
#define GL_FWD(name) guest::reg(#name, wrap(host_fn<decltype(&name)>(g_gles, #name)))

void register_gles() {
    std::string bin = project_root() + "/port/bin/";
    g_gles = LoadLibraryA((bin + "libGLESv2.dll").c_str());
    g_egl = LoadLibraryA((bin + "libEGL.dll").c_str());
    if (!g_egl || !g_gles) fatal("cannot load ANGLE (libEGL.dll / libGLESv2.dll) from %s", bin.c_str());

    EGL_FWD(eglChooseConfig); EGL_FWD(eglCreateContext); EGL_FWD(eglCreatePbufferSurface); EGL_FWD(eglDestroyContext);
    EGL_FWD(eglDestroySurface); EGL_FWD(eglGetConfigAttrib); EGL_FWD(eglGetCurrentContext); EGL_FWD(eglGetCurrentDisplay);
    EGL_FWD(eglGetError); EGL_FWD(eglInitialize); EGL_FWD(eglMakeCurrent); EGL_FWD(eglQueryString);
    EGL_FWD(eglQuerySurface); EGL_FWD(eglTerminate);

    static auto real_get_display = host_fn<decltype(&eglGetDisplay)>(g_egl, "eglGetDisplay");
    static auto real_create_surface = host_fn<decltype(&eglCreateWindowSurface)>(g_egl, "eglCreateWindowSurface");
    static auto real_swap = host_fn<decltype(&eglSwapBuffers)>(g_egl, "eglSwapBuffers");
    static auto real_bind_api = host_fn<decltype(&eglBindAPI)>(g_egl, "eglBindAPI");
    static auto real_error = host_fn<decltype(&eglGetError)>(g_egl, "eglGetError");

    IMPORT("eglGetDisplay", [](void*) -> EGLDisplay {
        EGLDisplay d = real_get_display(EGL_DEFAULT_DISPLAY);
        real_bind_api(EGL_OPENGL_ES_API);
        return d;
    });
    IMPORT("eglCreateWindowSurface", [](EGLDisplay d, EGLConfig c, void*, const EGLint* attribs) -> EGLSurface {
        EGLSurface s = real_create_surface(d, c, static_cast<HWND>(platform::hwnd()), attribs);
        logf("[egl] window surface %p (error 0x%x)", s, s ? 0x3000 : real_error());
        return s;
    });
    IMPORT("eglSwapBuffers", [](Thread& t, EGLDisplay d, EGLSurface s) -> EGLBoolean {
        platform::run_game_thread_tasks(t);        // frame boundary on the game thread
        u64 n = ++g_frames;
        if (n <= 3 || n % 600 == 0) logf("[egl] frame %llu", static_cast<unsigned long long>(n));
        maybe_screenshot();
        EGLBoolean ok = real_swap(d, s);
        if (gfx::frame_stats) {
            static auto last = std::chrono::steady_clock::now(), window = last;
            static double worst = 0;
            static int frames = 0, slow = 0;
            auto now = std::chrono::steady_clock::now();
            double ms = std::chrono::duration<double, std::milli>(now - last).count();
            last = now;
            worst = std::max(worst, ms);
            frames++;
            if (ms > 25) slow++;
            double span = std::chrono::duration<double>(now - window).count();
            if (span >= 5) {
                logf("[frames] %.1f fps, worst frame %.0f ms, %d frames over 25 ms", frames / span, worst, slow);
                window = now; worst = 0; frames = 0; slow = 0;
            }
        }
        return ok;
    });
    IMPORT("eglGetProcAddress", lookup_by_name);

    // Diagnostics: what the driver advertises and which texture formats the game uploads.
    static auto real_get_string = host_fn<decltype(&glGetString)>(g_gles, "glGetString");
    static auto real_compressed = host_fn<decltype(&glCompressedTexImage2D)>(g_gles, "glCompressedTexImage2D");
    static auto real_tex = host_fn<decltype(&glTexImage2D)>(g_gles, "glTexImage2D");
    static auto real_gl_error = host_fn<decltype(&glGetError)>(g_gles, "glGetError");

    GL_FWD(glActiveTexture); GL_FWD(glAttachShader); GL_FWD(glBindBuffer); GL_FWD(glBindFramebuffer);
    GL_FWD(glBindRenderbuffer); GL_FWD(glBindTexture); GL_FWD(glBlendColor); GL_FWD(glBlendEquation);
    GL_FWD(glBlendFunc); GL_FWD(glBufferData); GL_FWD(glBufferSubData); GL_FWD(glCheckFramebufferStatus);
    GL_FWD(glClear); GL_FWD(glClearColor); GL_FWD(glClearDepthf); GL_FWD(glClearStencil); GL_FWD(glColorMask);
    GL_FWD(glCompileShader); GL_FWD(glCompressedTexImage2D); GL_FWD(glCompressedTexSubImage2D);
    GL_FWD(glCopyTexSubImage2D); GL_FWD(glCreateProgram); GL_FWD(glCreateShader); GL_FWD(glCullFace);
    GL_FWD(glDeleteBuffers); GL_FWD(glDeleteFramebuffers); GL_FWD(glDeleteProgram); GL_FWD(glDeleteRenderbuffers);
    GL_FWD(glDeleteShader); GL_FWD(glDeleteTextures); GL_FWD(glDepthFunc); GL_FWD(glDepthMask); GL_FWD(glDepthRangef);
    GL_FWD(glDisable); GL_FWD(glDisableVertexAttribArray); GL_FWD(glDrawArrays); GL_FWD(glDrawElements);
    GL_FWD(glEnable); GL_FWD(glEnableVertexAttribArray); GL_FWD(glFinish); GL_FWD(glFlush);
    GL_FWD(glFramebufferRenderbuffer); GL_FWD(glFramebufferTexture2D); GL_FWD(glFrontFace); GL_FWD(glGenBuffers);
    GL_FWD(glGenFramebuffers); GL_FWD(glGenRenderbuffers); GL_FWD(glGenTextures); GL_FWD(glGenerateMipmap);
    GL_FWD(glGetActiveAttrib); GL_FWD(glGetActiveUniform); GL_FWD(glGetAttribLocation); GL_FWD(glGetError);
    GL_FWD(glGetFloatv); GL_FWD(glGetIntegerv); GL_FWD(glGetProgramInfoLog); GL_FWD(glGetProgramiv);
    GL_FWD(glGetShaderInfoLog); GL_FWD(glGetShaderSource); GL_FWD(glGetShaderiv); GL_FWD(glGetString);
    GL_FWD(glGetUniformLocation); GL_FWD(glLineWidth); GL_FWD(glLinkProgram); GL_FWD(glPixelStorei);
    GL_FWD(glPolygonOffset); GL_FWD(glReadPixels); GL_FWD(glRenderbufferStorage); GL_FWD(glSampleCoverage);
    GL_FWD(glScissor); GL_FWD(glShaderSource); GL_FWD(glStencilFunc); GL_FWD(glStencilMask); GL_FWD(glStencilOp);
    GL_FWD(glTexImage2D); GL_FWD(glTexParameterf); GL_FWD(glTexParameteri); GL_FWD(glTexSubImage2D);
    GL_FWD(glUniform1f); GL_FWD(glUniform1fv); GL_FWD(glUniform1i); GL_FWD(glUniform1iv); GL_FWD(glUniform2fv);
    GL_FWD(glUniform2iv); GL_FWD(glUniform3fv); GL_FWD(glUniform3iv); GL_FWD(glUniform4f); GL_FWD(glUniform4fv);
    GL_FWD(glUniform4iv); GL_FWD(glUniformMatrix2fv); GL_FWD(glUniformMatrix3fv); GL_FWD(glUniformMatrix4fv);
    GL_FWD(glUseProgram); GL_FWD(glVertexAttrib4f); GL_FWD(glVertexAttribPointer); GL_FWD(glViewport);

    // ETC1: the game decodes ETC1 textures on the CPU (slow under translation) unless the
    // driver advertises the ETC1 extension. ANGLE's ES3 context always accepts ETC2, of
    // which ETC1 is a strict subset, so advertise ETC1 and upload it as ETC2 RGB8.
    static auto real_compressed_sub = host_fn<decltype(&glCompressedTexSubImage2D)>(g_gles, "glCompressedTexSubImage2D");
    static auto real_storage = host_fn<decltype(&glTexStorage2D)>(g_gles, "glTexStorage2D");
    static auto real_sub = host_fn<decltype(&glTexSubImage2D)>(g_gles, "glTexSubImage2D");
    static auto etc = [](GLenum fmt) -> GLenum { return fmt == 0x8D64 ? 0x9274 : fmt; };   // ETC1_RGB8_OES -> COMPRESSED_RGB8_ETC2
    static auto note = [](const char* what, GLenum fmt, GLsizei w, GLsizei h) {
        log_once(std::string(what) + std::to_string(fmt), "[gl] first %s: format 0x%x, %dx%d (gl error 0x%x)", what, fmt, w, h, real_gl_error());
    };
    IMPORT("glGetString", [](GLenum name) -> const GLubyte* {
        const GLubyte* r = real_get_string(name);
        if (name == GL_EXTENSIONS && r) {
            static std::string ext;
            static std::once_flag once;
            std::call_once(once, [&] {
                ext = reinterpret_cast<const char*>(r);
                if (ext.find("GL_OES_compressed_ETC1_RGB8_texture") == std::string::npos) ext += " GL_OES_compressed_ETC1_RGB8_texture";
                logf("[gl] %zu characters of extensions (ETC1 added)", ext.size());
            });
            return reinterpret_cast<const GLubyte*>(ext.c_str());
        }
        log_once("glstr:" + std::to_string(name), "[gl] glGetString(0x%x) = %.200s", name, r ? reinterpret_cast<const char*>(r) : "(null)");
        return r;
    });
    IMPORT("glCompressedTexImage2D", [](GLenum target, GLint level, GLenum fmt, GLsizei w, GLsizei h, GLint border, GLsizei size, const void* data) {
        real_compressed(target, level, etc(fmt), w, h, border, size, data);
        note("compressed texture upload", fmt, w, h);
    });
    IMPORT("glCompressedTexSubImage2D", [](GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLsizei size, const void* data) {
        real_compressed_sub(target, level, x, y, w, h, etc(fmt), size, data);
        note("compressed texture update", fmt, w, h);
    });
    IMPORT("glTexStorage2D", [](GLenum target, GLsizei levels, GLenum fmt, GLsizei w, GLsizei h) {
        real_storage(target, levels, etc(fmt), w, h);
        note("texture storage", fmt, w, h);
    });
    IMPORT("glTexImage2D", [](GLenum target, GLint level, GLint ifmt, GLsizei w, GLsizei h, GLint border, GLenum fmt, GLenum type, const void* data) {
        real_tex(target, level, ifmt, w, h, border, fmt, type, data);
        note("plain texture upload", static_cast<GLenum>(ifmt), w, h);
    });
    IMPORT("glTexSubImage2D", [](GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h, GLenum fmt, GLenum type, const void* data) {
        real_sub(target, level, x, y, w, h, fmt, type, data);
        note("plain texture update", fmt, w, h);
    });
}
