// Android-specific services: assets, logging, the native window, sensors, and
// the Java methods (AndroidUtils and friends) that the game calls through JNI.
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <random>

#include "jni.h"
#include "libc.h"
#include "platform.h"

namespace fs = std::filesystem;

namespace {
struct Asset { std::vector<u8> data; size_t pos = 0; };

std::vector<u8> read_file(const std::string& path, bool* ok = nullptr) {
    std::ifstream f(path, std::ios::binary);
    if (ok) *ok = static_cast<bool>(f);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string asset_path(const char* name) {
    std::string n = name ? name : "";
    while (!n.empty() && (n[0] == '.' || n[0] == '/')) n = n.substr(1);
    return project_root() + "/work/apk/assets/" + n;
}

// ---- preferences: "group<TAB>key<TAB>value" lines ---------------------------------
std::mutex g_prefs_mutex;
std::map<std::pair<std::string, std::string>, std::string> g_prefs;
std::string prefs_file() { return project_root() + "/work/fs/prefs.txt"; }

void load_prefs() {
    std::ifstream f(prefs_file());
    std::string line;
    while (std::getline(f, line)) {
        size_t a = line.find('\t'), b = line.find('\t', a + 1);
        if (a == std::string::npos || b == std::string::npos) continue;
        g_prefs[{line.substr(0, a), line.substr(a + 1, b - a - 1)}] = line.substr(b + 1);
    }
}
void save_prefs() {
    std::ofstream f(prefs_file());
    for (auto& [k, v] : g_prefs) f << k.first << '\t' << k.second << '\t' << v << '\n';
}
void pref_set(const std::vector<JArg>& a, const std::string& value) {
    std::lock_guard lk(g_prefs_mutex);
    g_prefs[{jni::to_string(a[1].i), jni::to_string(a[0].i)}] = value;
    save_prefs();
}
bool pref_get(const std::vector<JArg>& a, std::string* out) {
    std::lock_guard lk(g_prefs_mutex);
    auto it = g_prefs.find({jni::to_string(a[1].i), jni::to_string(a[0].i)});
    if (it == g_prefs.end()) return false;
    *out = it->second;
    return true;
}

// The game's device-profile file caps the frame rate at 30 (allowed range 8..35)
// and leaves anisotropic filtering off. A PC can do better: rewrite those defaults
// as the file is handed to the game.
void patch_game_options(std::vector<u8>& bytes) {
    std::string s(bytes.begin(), bytes.end());
    auto set_default = [&](const char* key, const char* old_value, const std::string& new_value, const char* old_range,
                           const std::string& new_range) {
        size_t k = s.find(std::string("\"") + key + "\"");
        if (k == std::string::npos) return false;
        size_t v = s.find(old_value, k);
        if (v == std::string::npos || v - k > 80) return false;
        s.replace(v, strlen(old_value), new_value);
        size_t r = s.find(old_range, v);
        if (r != std::string::npos && r - v < 160) s.replace(r, strlen(old_range), new_range);
        return true;
    };
    int fps = platform::target_fps();
    bool ok = set_default("Max_FPS", "30", std::to_string(fps), "[8;35]", "[8;" + std::to_string(std::max(fps, 35)) + "]");
    set_default("Anisotropy", "0", "8", "[0;8]", "[0;8]");
    logf("[options] frame-rate cap set to %d fps%s", fps, ok ? "" : " (FAILED: Max_FPS default not found)");
    bytes.assign(s.begin(), s.end());
}

std::string uuid4() {
    static std::mt19937_64 rng{std::random_device{}()};
    char buf[40];
    u64 a = rng(), b = rng();
    snprintf(buf, sizeof buf, "%08x-%04x-4%03x-%04x-%012llx", static_cast<u32>(a >> 32), static_cast<u32>(a >> 16) & 0xFFFF,
             static_cast<u32>(a) & 0xFFF, (static_cast<u32>(b >> 48) & 0x3FFF) | 0x8000,
             static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFull));
    return buf;
}
}  // namespace

void register_android() {
    // ---- logging ------------------------------------------------------------------
    guest::reg("__android_log_print", [](Thread& t) {
        RegArgs a(t, 3);
        std::string s = format_guest(reinterpret_cast<const char*>(t.x(2)), a);
        while (!s.empty() && s.back() == '\n') s.pop_back();
        logf("[log %s] %s", reinterpret_cast<const char*>(t.x(1)), s.c_str());
        t.setx(0, 0);
    });

    // ---- assets -------------------------------------------------------------------
    IMPORT("AAssetManager_fromJava", [](void*, void*) -> void* { static int token; return &token; });
    IMPORT("AAssetManager_open", [](void*, const char* name, int) -> Asset* {
        bool ok;
        auto data = read_file(asset_path(name), &ok);
        if (!ok) { log_once(std::string("asset:") + name, "[asset] %s -> missing", name); return nullptr; }
        auto* a = new Asset;
        a->data = std::move(data);
        return a;
    });
    IMPORT("AAsset_getLength", [](Asset* a) -> i64 { return static_cast<i64>(a->data.size()); });
    IMPORT("AAsset_read", [](Asset* a, void* buf, size_t n) -> int {
        size_t k = std::min(n, a->data.size() - a->pos);
        memcpy(buf, a->data.data() + a->pos, k);
        a->pos += k;
        return static_cast<int>(k);
    });
    IMPORT("AAsset_seek", [](Asset* a, i64 off, int whence) -> i64 {
        i64 base = whence == 0 ? 0 : whence == 1 ? static_cast<i64>(a->pos) : static_cast<i64>(a->data.size());
        a->pos = static_cast<size_t>(std::clamp<i64>(base + off, 0, static_cast<i64>(a->data.size())));
        return static_cast<i64>(a->pos);
    });
    IMPORT("AAsset_getBuffer", [](Asset* a) -> const void* { return a->data.data(); });
    IMPORT("AAsset_close", [](Asset* a) { delete a; });
    IMPORT("AAssetManager_openDir", [](void*, const char*) -> void* { return nullptr; });
    IMPORT("AAssetDir_getNextFileName", [](void*) -> const char* { return nullptr; });
    IMPORT("AAssetDir_rewind", [](void*) {});
    IMPORT("AAssetDir_close", [](void*) {});

    // ---- window, looper, sensors ------------------------------------------------------
    IMPORT("ANativeWindow_fromSurface", [](void*, void*) -> void* { return platform::window_token(); });
    IMPORT("ANativeWindow_getWidth", [](void*) -> int { return platform::width(); });
    IMPORT("ANativeWindow_getHeight", [](void*) -> int { return platform::height(); });
    IMPORT("ANativeWindow_setBuffersGeometry", [](void*, int, int, int) -> int { return 0; });
    IMPORT("ANativeWindow_release", [](void*) {});
    auto token = []() -> void* { static int tok; return &tok; };
    for (const char* n : {"ALooper_forThread", "ALooper_prepare", "ASensorManager_getInstance", "ASensorManager_createEventQueue"})
        guest::reg(n, wrap(+token));
    for (const char* n : {"ASensorManager_getDefaultSensor", "ASensorEventQueue_enableSensor", "ASensorEventQueue_disableSensor",
                          "ASensorEventQueue_setEventRate", "ASensorEventQueue_getEvents"})
        guest::reg(n, [](Thread& t) { t.setx(0, 0); });
}

// ---- virtual keyboard state (set by the game thread, typed into by the UI thread) ----
namespace {
std::mutex g_kb_mutex;
bool g_kb_visible = false;
bool g_kb_numeric = false;
int g_kb_max = 0;
std::string g_kb_text;

// Mirrors VirtualKeyboard.onTextChanged: numeric fields keep leading digits only,
// and a leading zero collapses to "-".
bool kb_apply(std::string text, std::string* out) {
    if (g_kb_numeric) {
        std::string digits;
        for (char c : text) { if (c < '0' || c > '9') break; digits += c; }
        text = !digits.empty() && digits[0] == '0' ? "-" : digits;
    }
    if (g_kb_max > 0 && static_cast<int>(text.size()) > g_kb_max) text.resize(g_kb_max);
    if (text == g_kb_text) return false;
    g_kb_text = text;
    *out = text;
    return true;
}
}  // namespace

bool keyboard::visible() { std::lock_guard lk(g_kb_mutex); return g_kb_visible; }
void keyboard::hide() { std::lock_guard lk(g_kb_mutex); g_kb_visible = false; }
bool keyboard::on_text(const char* utf8, std::string* out) {
    std::lock_guard lk(g_kb_mutex);
    if (!g_kb_visible) return false;
    std::string base = g_kb_text == "-" ? "" : g_kb_text;
    return kb_apply(base + utf8, out);
}
bool keyboard::on_backspace(std::string* out) {
    std::lock_guard lk(g_kb_mutex);
    if (!g_kb_visible || g_kb_text.empty()) return false;
    std::string t = g_kb_text == "-" ? "" : g_kb_text;
    if (!t.empty()) t.pop_back();
    if (t == g_kb_text) return false;
    g_kb_text = t;
    *out = t;
    return true;
}

void register_java_methods() {
    const std::string pkg = PKG;
    const std::string utils = "com/gameloft/android/ANMP/GloftNOHM/PackageUtils/AndroidUtils";
    auto constant = [&](const std::string& cls, const char* name, JRet v) {
        jni::define(cls, name, [v](const std::vector<JArg>&) { return v; });
    };
    const std::pair<const char*, std::string> strings[] = {
        {"RetrieveSDCardPath", "/sdcard"},
        {"RetrieveObbPath", "NA"},
        {"RetrieveDataPath", "/data/data/" + pkg + "/files"},
        {"RetrieveSavePath", "/data/data/" + pkg + "/files"},
        {"RetrieveTempPath", "/data/data/" + pkg + "/cache"},
        {"RetrieveNativeLibraryPath", "/data/app/" + pkg + "-1/lib/arm64"},
        {"GetApkPath", "/data/app/" + pkg + "-1/base.apk"},
        {"GetLibSoPath", "/data/app/" + pkg + "-1/lib/arm64/libNOVA.so"},
        {"GetAndroidID", "9774d56d682e549c"},
        {"GetSerial", "unknown"},
        {"GetCPUSerial", "0000000000000000"},
        {"GetDeviceManufacturer", "Google"},
        {"GetCPUAbi", "arm64-v8a"},
        {"GetDeviceModel", "Pixel 2"},
        {"GetPhoneProduct", "walleye"},
        {"GetPhoneDevice", "walleye"},
        {"GetBuildBoard", "walleye"},
        {"GetFirmware", "10"},
        {"GetMacAddress", "02:00:00:00:00:00"},
        {"GetDeviceIMEI", ""},
        {"GetHDIDFV", "6f1c2a34-5b6d-4e7f-8a9b-0c1d2e3f4a5b"},
        {"GetDeviceLanguage", "en"},
        {"GetCountry", "US"},
        {"GetDeviceSettingsCountryCode", "US"},
        {"GetSimIsoCountryCode", "us"},
    };
    for (auto& [name, value] : strings) constant(utils, name, JRet(value));
    constant(utils, "GetXDpi", JRet(160.0));
    constant(utils, "GetYDpi", JRet(160.0));
    constant(utils, "initCheckConnectionType", JRet(0));
    constant(utils, "GetDiskFreeSpace", JRet(20000000000ll));      // bytes
    constant(utils, "GetDiskTotalSpace", JRet(64000));             // megabytes
    constant(utils, "GetGameName", JRet("ANMP.GloftNOHM"));
    constant(utils, "GetUserAgent", JRet("Mozilla/5.0 (Linux; Android 10; Pixel 2)"));
    constant("com/gameloft/android/ANMP/GloftNOHM/PackageUtils/EmulatorDetector", "IsGameRunOnEmulator", JRet(false));
    jni::define("com/gameloft/android/ANMP/GloftNOHM/GLUtils/SUtils", "GenerateUUID",
                [](const std::vector<JArg>&) { return JRet(uuid4()); });
    jni::define(utils, "GetElapsedRealtime", [](const std::vector<JArg>&) { return JRet(static_cast<long long>(GetTickCount64())); });

    // The APK's own signing certificate hash, as Signature.hashCode() computes it.
    jni::define(utils, "retrieveBarrels", [](const std::vector<JArg>&) {
        auto cert = read_file(project_root() + "/work/cert.der");
        u32 h = 1;
        for (u8 b : cert) h = 31 * h + static_cast<u32>(static_cast<i32>(static_cast<signed char>(b)));
        auto* a = new JObj;
        a->cls = "[I";
        a->elem_size = 4;
        a->bytes.resize(16);
        memcpy(a->bytes.data(), &h, 4);
        return JRet(a);
    });
    jni::define(utils, "GetAssetAsString", [](const std::vector<JArg>& a) {
        auto* arr = new JObj;
        arr->cls = "[B";
        arr->elem_size = 1;
        std::string name = jni::to_string(a[0].i);
        arr->bytes = read_file(asset_path(name.c_str()));
        if (name.find("GO_android.json") != std::string::npos) patch_game_options(arr->bytes);
        return JRet(arr);
    });

    // Virtual keyboard: ShowKeyboard(initialText, inputType, ?, ?, maxLength)
    jni::define(utils, "ShowKeyboard", [](const std::vector<JArg>& a) {
        std::lock_guard lk(g_kb_mutex);
        g_kb_visible = true;
        g_kb_text = jni::to_string(a[0].i);
        g_kb_numeric = static_cast<i32>(a[1].i) == 2 || static_cast<i32>(a[1].i) == 3;
        g_kb_max = static_cast<i32>(a[4].i);
        logf("[keyboard] shown (%s, max %d): type on your keyboard, Enter to finish", g_kb_numeric ? "numbers" : "text", g_kb_max);
        return JRet();
    });
    jni::define(utils, "HideKeyboard", [](const std::vector<JArg>&) { keyboard::hide(); return JRet(); });
    jni::define(utils, "IsKeyboardVisible", [](const std::vector<JArg>&) { return JRet(keyboard::visible()); });
    jni::define(utils, "GetVKeyboardText", [](const std::vector<JArg>&) { std::lock_guard lk(g_kb_mutex); return JRet(g_kb_text); });
    jni::define(utils, "SetVKeyboardText", [](const std::vector<JArg>& a) {
        std::lock_guard lk(g_kb_mutex);
        g_kb_text = jni::to_string(a[0].i);
        return JRet();
    });

    // SharedPreferences: (key, group, value-or-default)
    load_prefs();
    jni::define(utils, "SavePreferenceString", [](const std::vector<JArg>& a) { pref_set(a, jni::to_string(a[2].i)); return JRet(); });
    jni::define(utils, "SavePreferenceInt", [](const std::vector<JArg>& a) { pref_set(a, std::to_string(static_cast<i32>(a[2].i))); return JRet(); });
    jni::define(utils, "SavePreferenceLong", [](const std::vector<JArg>& a) { pref_set(a, std::to_string(static_cast<i64>(a[2].i))); return JRet(); });
    jni::define(utils, "SavePreferenceBool", [](const std::vector<JArg>& a) { pref_set(a, (a[2].i & 0xFF) ? "1" : "0"); return JRet(); });
    jni::define(utils, "GetPreferenceString", [](const std::vector<JArg>& a) {
        std::string v;
        return JRet(pref_get(a, &v) ? v : jni::to_string(a[2].i));
    });
    jni::define(utils, "GetPreferenceInt", [](const std::vector<JArg>& a) {
        std::string v;
        return JRet(pref_get(a, &v) ? atoi(v.c_str()) : static_cast<i32>(a[2].i));
    });
    jni::define(utils, "GetPreferenceLong", [](const std::vector<JArg>& a) {
        std::string v;
        return JRet(pref_get(a, &v) ? atoll(v.c_str()) : static_cast<long long>(a[2].i));
    });
    jni::define(utils, "GetPreferenceBool", [](const std::vector<JArg>& a) {
        std::string v;
        return JRet(pref_get(a, &v) ? v == "1" : (a[2].i & 0xFF) != 0);
    });
    jni::define(utils, "RemovePreference", [](const std::vector<JArg>& a) {
        std::lock_guard lk(g_prefs_mutex);
        g_prefs.erase({jni::to_string(a[1].i), jni::to_string(a[0].i)});
        save_prefs();
        return JRet();
    });

    jni::define_static_field("android/os/Build", "MODEL", JRet("Pixel 2"));
    jni::define_static_field("android/os/Build", "MANUFACTURER", JRet("Google"));
    jni::define_static_field("android/os/Build", "PRODUCT", JRet("walleye"));
    jni::define_static_field("android/os/Build", "DEVICE", JRet("walleye"));
    jni::define_static_field("android/os/Build", "BRAND", JRet("google"));
    jni::define_static_field("android/os/Build", "HARDWARE", JRet("qcom"));
}
