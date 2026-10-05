// N.O.V.A. Legacy loader: runs the Android game's ARM64 library on Windows.
// The host main thread plays the role of Android's Java UI thread: it boots
// the library the way MainActivity does, then pumps window events into it.
#define SDL_MAIN_HANDLED
#include <windows.h>
#include <dbghelp.h>

#include <SDL.h>
#include <SDL_syswm.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <csignal>
#include <exception>
#include <thread>

#include "controls.h"
#include "guest.h"
#include "interp.h"
#include "jni.h"
#include "libc.h"
#include "platform.h"

// Ask hybrid-graphics laptops for the fast GPU.
extern "C" {
__declspec(dllexport) DWORD NvOptimusEnablement = 1;
__declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 1;
}

namespace {
// --profile: sample the main thread's instruction pointer during startup and
// report which host functions it was in.
struct Sampler {
    std::thread th;
    std::atomic<bool> stop{false};
    std::map<std::string, int> hits;
    int total = 0;
    void start(HANDLE target = nullptr) {
        if (!target) DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &target, 0, FALSE, DUPLICATE_SAME_ACCESS);
        SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(GetCurrentProcess(), nullptr, TRUE);
        th = std::thread([this, target] {
            while (!stop) {
                Sleep(1);
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_CONTROL;
                SuspendThread(target);
                GetThreadContext(target, &ctx);
                ResumeThread(target);
                char buf[sizeof(SYMBOL_INFO) + 256];
                auto* si = reinterpret_cast<SYMBOL_INFO*>(buf);
                si->SizeOfStruct = sizeof(SYMBOL_INFO);
                si->MaxNameLen = 255;
                DWORD64 disp = 0;
                std::string name = SymFromAddr(GetCurrentProcess(), ctx.Rip, &disp, si) ? si->Name : "<jit code or unknown>";
                hits[name.substr(0, 110)]++;
                total++;
            }
        });
    }
    void report() {
        stop = true;
        th.join();
        stop = false;
        std::vector<std::pair<int, std::string>> v;
        for (auto& [n, c] : hits) v.emplace_back(c, n);
        std::sort(v.rbegin(), v.rend());
        logf("--- profile: %d samples ---", total);
        hits.clear();
        for (size_t i = 0; i < v.size() && i < 25; i++) logf("  %5.1f%%  %s", 100.0 * v[i].first / total, v[i].second.c_str());
    }
};

SDL_Window* g_window = nullptr;
HWND g_hwnd = nullptr;
int g_width = 1280, g_height = 720;

LONG WINAPI crash_filter(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* r = info->ExceptionRecord;
    logf("\n** host exception 0x%08lx at %p", r->ExceptionCode, r->ExceptionAddress);
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2)
        logf("   %s address 0x%llx", r->ExceptionInformation[0] ? "writing" : "reading",
             static_cast<unsigned long long>(r->ExceptionInformation[1]));
    if (Thread* t = Thread::current_or_null()) guest::backtrace(*t);
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 4);
    return EXCEPTION_EXECUTE_HANDLER;
}

// First-chance handler: with identity-mapped guest memory any access violation
// is a real bug (a bad guest pointer), so report it before anything else runs.
LONG WINAPI first_chance(EXCEPTION_POINTERS* info) {
    DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_STACK_OVERFLOW ||
        code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == EXCEPTION_PRIV_INSTRUCTION)
        return crash_filter(info);
    return EXCEPTION_CONTINUE_SEARCH;
}
}  // namespace

void* platform::window_token() { return g_hwnd; }
void* platform::hwnd() { return g_hwnd; }
int platform::width() { return g_width; }
int platform::height() { return g_height; }
static void* g_game_thread = nullptr;
static int g_target_fps = 60;
int platform::target_fps() { return g_target_fps; }
void* platform::game_thread_handle() { return g_game_thread; }
void platform::set_game_thread_handle(void* h) { g_game_thread = h; }

int main(int argc, char** argv) {
    std::string script_path;
    bool bench = false, profile = false, guest_profile = false;
    bool headless_seconds = false;
    int run_seconds = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--trace")) guest::trace = true;
        if (!strcmp(argv[i], "--bench")) bench = true;
        if (!strcmp(argv[i], "--frame-stats")) gfx::frame_stats = true;
        if (!strcmp(argv[i], "--guest-profile")) guest_profile = true;
        if (!strcmp(argv[i], "--fps") && i + 1 < argc) g_target_fps = std::max(8, atoi(argv[++i]));
        if (!strcmp(argv[i], "--no-interp")) interp::enabled = false;
        if (!strcmp(argv[i], "--verify-interp")) guest::enable_interp_verify();
        if (!strcmp(argv[i], "--hot") && i + 1 < argc) interp::hot_threshold = static_cast<u32>(atoi(argv[++i]));
        if (!strcmp(argv[i], "--profile")) profile = true;
        if (!strcmp(argv[i], "--script") && i + 1 < argc) script_path = argv[++i];
        if (!strcmp(argv[i], "--shot-every") && i + 1 < argc) gfx::shot_every = atof(argv[++i]);
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) { run_seconds = atoi(argv[++i]); headless_seconds = true; }
    }
    signal(SIGABRT, [](int) { fatal("** host abort() (an internal assertion failed; see the message above)"); });
    std::set_terminate([] { fatal("** unhandled C++ exception in the loader"); });
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    // Bad arguments to CRT functions should fail the call, not kill the process.
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t* fn, const wchar_t*, unsigned, uintptr_t) {
        logf("!! invalid parameter passed to host CRT function %ls", fn ? fn : L"?");
    });
    SetUnhandledExceptionFilter(crash_filter);
    AddVectoredExceptionHandler(1, first_chance);
    logf("N.O.V.A. Legacy loader, project root %s", project_root().c_str());

    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) fatal("SDL_Init: %s", SDL_GetError());
    g_window = SDL_CreateWindow("N.O.V.A. Legacy", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, g_width, g_height, 0);
    if (!g_window) fatal("SDL_CreateWindow: %s", SDL_GetError());
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    SDL_GetWindowWMInfo(g_window, &wm);
    g_hwnd = wm.info.win.window;

    register_libc();
    register_stdio();
    register_pthread();
    register_android();
    register_gles();
    register_audio();
    register_net();
    jni::init();
    register_java_methods();

    auto t0 = std::chrono::steady_clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
    guest::load(project_root() + "/work/lib/arm64-v8a/libNOVA.so");
    install_hle();
    logf("loaded libNOVA.so at %llx (%.2fs)", static_cast<unsigned long long>(guest::image_base()), elapsed());

    logf("process memory before first thread: %.0f MB", guest::private_mb());
    Thread ui("java-ui", 4 << 20, 96);
    logf("process memory after first thread: %.0f MB", guest::private_mb());
    ui.make_current();
    Sampler sampler;
    if (profile) sampler.start();
    if (bench) {
        // First call translates the function; later calls only run it.
        if (u64 fn = guest::sym("adler32")) {
            static u8 buf[4096];
            for (int round = 0; round < 3; round++) {
                double before = elapsed();
                ui.call(fn, {1, reinterpret_cast<u64>(buf), sizeof buf});
                logf("bench: adler32 call %d took %.3f ms", round, (elapsed() - before) * 1e3);
            }
        }
        // Cost of one guest->host import round trip (two JIT entries per call).
        u64 stub = guest::make_stub("bench", [](Thread& t) { t.setx(0, 1); });
        double before = elapsed();
        for (int i = 0; i < 200000; i++) ui.call(stub);
        logf("bench: %.2f microseconds per import round trip", (elapsed() - before) * 1e6 / 200000);
    }
    for (u64 fn : guest::init_array()) {
        if (!fn) continue;
        double before = elapsed();
        ui.call(fn);
        if (elapsed() - before > 0.2) logf("slow constructor: %s took %.2fs", guest::symbolize(fn).c_str(), elapsed() - before);
    }
    if (getenv("NOVA_JIT_STATS")) logf("jit stats: %llu instructions translated, %llu distinct", static_cast<unsigned long long>(ui.translated), static_cast<unsigned long long>(ui.unique_translated()));
    if (profile) sampler.report();
    logf("static constructors done (%.2fs; %llu import calls, %llu instructions interpreted, %llu translated)", elapsed(),
         static_cast<unsigned long long>(ui.svc_count), static_cast<unsigned long long>(ui.interpreted),
         static_cast<unsigned long long>(ui.translated));
    if (getenv("NOVA_JIT_STATS")) interp::report_unsupported();

    const std::string bridge = "Java_com_gameloft_android_ANMP_GloftNOHM_PackageUtils_JNIBridge_";
    u64 cls = jni::find_class("com/gameloft/android/ANMP/GloftNOHM/PackageUtils/JNIBridge");
    auto native = [&](const char* name, std::initializer_list<u64> extra = {}) {
        u64 fn = guest::sym(bridge + name);
        if (!fn) fatal("missing native %s", name);
        std::vector<u64> args = {jni::env(), cls};
        args.insert(args.end(), extra);
        switch (args.size()) {
        case 2: return ui.call(fn, {args[0], args[1]});
        case 3: return ui.call(fn, {args[0], args[1], args[2]});
        case 4: return ui.call(fn, {args[0], args[1], args[2], args[3]});
        default: return ui.call(fn, {args[0], args[1], args[2], args[3], args[4]});
        }
    };

    ui.call(guest::sym("JNI_OnLoad"), {jni::vm(), 0});
    logf("JNI_OnLoad done (%.2fs)", elapsed());
    native("NativeInit");
    logf("NativeInit done (%.2fs)", elapsed());
    std::this_thread::sleep_for(std::chrono::milliseconds(600));    // let the game thread reach its event loop
    native("NativeOnResume");
    native("NativeSurfaceChanged", {jni::new_object("android/view/Surface"), static_cast<u64>(g_width), static_cast<u64>(g_height)});
    logf("surface delivered (%.2fs)", elapsed());
    if (guest_profile) guest::start_guest_profile(2);      // thread 2 is the game's main loop
    Sampler game_sampler;
    if (profile && platform::game_thread_handle()) game_sampler.start(platform::game_thread_handle());

    auto touch = [&](int action, float x, float y, int id) {
        ui.sets(0, x);
        ui.sets(1, y);
        native("NativeOnTouch", {static_cast<u64>(action), static_cast<u64>(id)});
    };

    // Scripted touches for unattended test runs: lines of "<seconds> <down|move|up|tap> <x> <y> [finger]".
    struct Step { double at; int action; float x, y; int id; };
    std::vector<Step> script;
    if (!script_path.empty()) {
        if (FILE* f = fopen(script_path.c_str(), "r")) {
            char line[256], verb[16];
            while (fgets(line, sizeof line, f)) {
                double at; float x, y; int id = 0;
                int fields = sscanf(line, "%lf %15s %f %f %d", &at, verb, &x, &y, &id);
                std::string v = fields >= 2 ? verb : "";
                if (v == "pack" && fields >= 3) { script.push_back({at, 100, x, 0, 0}); continue; }
                if (fields < 4) continue;
                if (v == "tap") { script.push_back({at, 0, x, y, id}); script.push_back({at + 0.12, 2, x, y, id}); }
                else script.push_back({at, v == "down" ? 0 : v == "move" ? 1 : 2, x, y, id});
            }
            fclose(f);
        }
        std::stable_sort(script.begin(), script.end(), [](const Step& a, const Step& b) { return a.at < b.at; });
    }
    size_t script_pos = 0;

    packs::init();
    SDL_StartTextInput();
    Controls controls(g_window, touch);
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { running = false; continue; }
            if (ev.type == SDL_KEYDOWN && !ev.key.repeat && ev.key.keysym.sym >= SDLK_F1 && ev.key.keysym.sym <= SDLK_F9) {
                packs::request(ev.key.keysym.sym - SDLK_F1);
                continue;
            }
            if (controls.handle(ev, keyboard::visible())) continue;
            switch (ev.type) {
            case SDL_TEXTINPUT: {
                std::string text;
                if (keyboard::on_text(ev.text.text, &text)) native("NativeSendKeyboardData", {jni::new_string(text)});
                break;
            }
            case SDL_KEYDOWN:
            case SDL_KEYUP:
                if (ev.type == SDL_KEYDOWN && keyboard::visible()) {
                    std::string text;
                    if (ev.key.keysym.sym == SDLK_BACKSPACE && keyboard::on_backspace(&text))
                        native("NativeSendKeyboardData", {jni::new_string(text)});
                    if (ev.key.keysym.sym == SDLK_RETURN || ev.key.keysym.sym == SDLK_KP_ENTER) keyboard::hide();
                }
                if (ev.key.keysym.sym == SDLK_ESCAPE && !ev.key.repeat)
                    native("NativeKeyAction", {4, static_cast<u64>(ev.type == SDL_KEYDOWN)});   // Android "back"
                break;
            }
        }
        controls.tick();
        while (script_pos < script.size() && elapsed() >= script[script_pos].at) {
            const Step& st = script[script_pos++];
            if (st.action == 100) packs::request(static_cast<int>(st.x) - 1);
            else touch(st.action, st.x, st.y, st.id);
        }
        if (headless_seconds && elapsed() > run_seconds) running = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    // Android pauses an app before it goes away, and the game saves its profile then.
    logf("closing: pausing the game so it can save");
    native("NativeOnPause");
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    if (profile && platform::game_thread_handle()) { logf("game thread:"); game_sampler.report(); }
    if (guest_profile) guest::report_guest_profile(2);
    guest::dump_threads();
    logf("exiting after %.1fs", elapsed());
    fflush(stdout);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}
