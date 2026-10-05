// Native replacements for a few guest functions that profiling showed to be
// hot and trivially simple. Each is tied to this exact build of libNOVA.so and
// is only installed if the code at that address is what we expect.
#include <windows.h>

#include "guest.h"
#include "platform.h"

namespace {
// Replace the first instruction of a guest function with a service call to `fn`.
bool hook(u64 offset, u32 expected_first_insn, const char* name, Handler fn) {
    u32* at = reinterpret_cast<u32*>(guest::image_base() + offset);
    if (*at != expected_first_insn) {
        logf("[hle] %s not installed: unexpected code at 0x%llx", name, static_cast<unsigned long long>(offset));
        return false;
    }
    u64 stub = guest::make_stub(std::string("hle:") + name, std::move(fn));
    *at = *reinterpret_cast<u32*>(stub);       // the stub's own `svc #n` instruction
    return true;
}
// libc++ std::string as laid out in the guest (short-string optimisation).
std::string guest_string(u64 addr) {
    if (!addr) return "";
    const u8* p = reinterpret_cast<const u8*>(addr);
    if (p[0] & 1) return std::string(*reinterpret_cast<const char* const*>(p + 16), *reinterpret_cast<const u64*>(p + 8));
    return std::string(reinterpret_cast<const char*>(p + 1), p[0] >> 1);
}

// Run `before` when a guest function is entered, then let the function continue.
// The first instruction is moved to a small trampoline, so it must not be PC-relative.
bool observe(const char* symbol, const char* name, Handler before) {
    u64 entry = guest::sym(symbol);
    if (!entry) { logf("[hle] %s: symbol not found", name); return false; }
    u32 first = *reinterpret_cast<u32*>(entry);
    bool pc_relative = (first & 0x1F000000) == 0x10000000 || (first & 0x7C000000) == 0x14000000 ||
                       (first & 0x3B000000) == 0x18000000 || (first & 0x7E000000) == 0x34000000 ||
                       (first & 0x7E000000) == 0x36000000 || (first & 0xFF000010) == 0x54000000;
    if (pc_relative) { logf("[hle] %s: cannot relocate first instruction %08x", name, first); return false; }
    u32* tramp = static_cast<u32*>(malloc(24));
    tramp[0] = first;
    tramp[1] = 0x58000050;                      // ldr x16, #8
    tramp[2] = 0xD61F0200;                      // br x16
    u64 back = entry + 4;
    memcpy(&tramp[3], &back, 8);
    u64 stub = guest::make_stub(std::string("hle:") + name, [before, tramp](Thread& t) {
        before(t);
        t.set_pc(reinterpret_cast<u64>(tramp));
    });
    *reinterpret_cast<u32*>(entry) = *reinterpret_cast<u32*>(stub);
    return true;
}
// Replace a whole guest function: `body` runs instead of it and the function returns.
bool replace(const char* symbol, const char* name, Handler body) {
    u64 entry = guest::sym(symbol);
    if (!entry) { logf("[hle] %s: symbol not found", name); return false; }
    u64 stub = guest::make_stub(std::string("hle:") + name, [body](Thread& t) {
        u64 ret = t.lr();
        body(t);
        t.set_pc(ret);
    });
    *reinterpret_cast<u32*>(entry) = *reinterpret_cast<u32*>(stub);
    return true;
}

// ---- offline rewards ---------------------------------------------------------------
// The game's "watch a video" buttons asked Gameloft's ad servers for an ad and then
// asked the game server how much to pay out. Neither exists any more, so pressing a
// button now pays the reward straight away through the game's own reward popup.
int g_free_trilithium = 25, g_free_coins = 1000, g_shop_trilithium = 50000;

void load_offline_config() {
    std::string path = project_root() + "/offline.cfg";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        if ((f = fopen(path.c_str(), "w"))) {
            fprintf(f, "# N.O.V.A. Legacy port, offline rewards. Amounts paid when a \"watch a video\" button is pressed.\n"
                       "free_trilithium = %d\nfree_coins = %d\n"
                       "# Paid each time the Trilithium tab of the store is opened (the purple + button).\n"
                       "shop_trilithium = %d\n",
                    g_free_trilithium, g_free_coins, g_shop_trilithium);
            fclose(f);
        }
        return;
    }
    char key[64];
    int value;
    char line[128];
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, " %63[a-z_] = %d", key, &value) != 2) continue;
        if (!strcmp(key, "free_trilithium")) g_free_trilithium = value;
        if (!strcmp(key, "free_coins")) g_free_coins = value;
        if (!strcmp(key, "shop_trilithium")) g_shop_trilithium = value;
    }
    fclose(f);
}

void install_video_rewards() {
    u64 singleton = guest::sym("_ZN16GameGLAdsManager9SingletonE");
    u64 set_type = guest::sym("_ZN16GameGLAdsManager13SetRewardTypeEPKc");
    u64 set_reward = guest::sym("_ZN16GameGLAdsManager9SetRewardEi");
    u64 show_popup = guest::sym("_ZN16GameGLAdsManager15ShowRewardPopopEv");
    if (!singleton || !set_type || !set_reward || !show_popup) { logf("[hle] video rewards not installed: symbols missing"); return; }

    struct Button { const char* handler; const char* reward_type; int* amount; };
    static int one = 1;
    static const Button buttons[] = {
        {"FreeCash", "1_Hard_Currency", &g_free_trilithium},
        {"FreeCoin", "1_Soft_Currency", &g_free_coins},
        {"GatchaShop", "1_free_gacha", &one},
        {"MainMenu", "1_free_gacha", &one},
        {"SupplySpin", "1_Spin", &one},
        {"FreeGrenade", "1_Grenade", &one},
        {"CardMarketplace", "1_Card", &one},
        {"Revive", "1_revive", &one},
        {"DoubleRewards", "Double_SC_earned", &one},
        {"SP_DoubleHC", "Double_HC_earned", &one},
    };
    for (const Button& b : buttons) {
        std::string name = std::string("onCommandWatchvideo_") + b.handler;
        std::string symbol = "_Z" + std::to_string(name.size()) + name + "RKN7gameswf18ASNativeEventStateE";
        replace(symbol.c_str(), b.handler, [=](Thread& t) {
            u64 manager = *reinterpret_cast<u64*>(singleton);
            if (!manager) return;
            logf("[offline] %s pressed: granting %s x%d", b.handler, b.reward_type, *b.amount);
            t.call(set_type, {manager, reinterpret_cast<u64>(b.reward_type)});
            t.call(set_reward, {manager, static_cast<u64>(*b.amount)});
            t.call(show_popup, {manager});
        });
    }

    // The Trilithium store tab asked Gameloft for its item list. It now pays out directly.
    replace("_Z15onAskForIAPDataRKN7gameswf18ASNativeEventStateE", "TrilithiumShop", [=](Thread& t) {
        static ULONGLONG last = 0;
        u64 manager = *reinterpret_cast<u64*>(singleton);
        ULONGLONG now = GetTickCount64();
        if (!manager || now - last < 1500) return;      // one grant per visit, not per refresh
        last = now;
        logf("[offline] Trilithium shop opened: granting %d trilithium", g_shop_trilithium);
        t.call(set_type, {manager, reinterpret_cast<u64>("1_Hard_Currency")});
        t.call(set_reward, {manager, static_cast<u64>(g_shop_trilithium)});
        t.call(show_popup, {manager});
    });
    observe("_ZN13BundleManager18InitializeFromFileERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE",
            "BundleManager.Init", [](Thread& t) { packs::set_bundle_manager(t.x(0)); });

    // Report every video button as available so the menus stop showing "Connecting...".
    int status = getenv("NOVA_AD_STATUS") ? atoi(getenv("NOVA_AD_STATUS")) : 1;
    observe("_ZN16GameGLAdsManager20SetButtonVideoStatusENS_6EVideoEi", "SetButtonVideoStatus",
            [status](Thread& t) { t.setx(2, static_cast<u64>(status)); });
}
}  // namespace

void install_hle() {
    if (getenv("NOVA_NO_HLE")) return;
    const u64 base = guest::image_base();

    // "Remove pointer from array": scans a pointer array backwards for a value and
    // tail-calls erase-at-index. Called once per element while levels load, so the
    // scan dominates (about 15% of load time under translation).
    //   0x1bd890c: ldrsw x9,[x0,#0x28] ; add x0,x0,#0x20 ; loop ... ; b 0x1bb42c0
    hook(0x1bd890c, 0xB9802809, "array_remove", [base](Thread& t) {
        u64 list = t.x(0) + 0x20, key = t.x(1);
        i32 n = *reinterpret_cast<i32*>(t.x(0) + 0x28);
        u64* items = *reinterpret_cast<u64**>(list);
        t.setx(0, list);
        for (i32 i = n - 1; i >= 0; i--) {
            if (items[i] == key) {
                t.setx(1, static_cast<u32>(i));
                t.set_pc(base + 0x1bb42c0);     // erase(list, index); it returns to our caller
                return;
            }
        }
        t.set_pc(t.lr());                       // not found: plain return
    });

    load_offline_config();
    install_video_rewards();

    if (getenv("NOVA_UI_TRACE")) {
        // Log every menu event the Flash UI sends to native code, and a few loaders.
        int n = 0;
        for (auto& [symbol, addr] : guest::exports_containing("RKN7gameswf18ASNativeEventStateE")) {
            size_t digits = 2;
            while (digits < symbol.size() && isdigit(static_cast<unsigned char>(symbol[digits]))) digits++;
            std::string name = symbol.substr(digits, symbol.find("RKN7gameswf") - digits);
            if (name.rfind("on", 0) != 0 || name.find("Update") != std::string::npos || name.find("Hud") != std::string::npos) continue;
            if (observe(symbol.c_str(), name.c_str(), [name](Thread&) { logf("[ui] %s", name.c_str()); })) n++;
        }
        logf("[hle] tracing %d menu events", n);
        observe("_ZN13BundleManager18InitializeFromFileERKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE",
                "BundleManager.InitializeFromFile", [](Thread& t) {
                    logf("[ui] BundleManager::InitializeFromFile(this=%llx, %s)", static_cast<unsigned long long>(t.x(0)), guest_string(t.x(1)).c_str());
                });
    }
    if (getenv("NOVA_ADS_TRACE")) {
        auto trace = [](const char* what, int string_reg) {
            return [what, string_reg](Thread& t) {
                logf("[ads-trace] %s(%s) from %s", what, string_reg >= 0 ? guest_string(t.x(string_reg)).c_str() : "",
                     guest::symbolize(t.lr()).c_str());
            };
        };
        const std::string str = "RKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEE";
        const std::string str0 = "RKNSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEE";
        observe(("_ZN6adslib24AdsManagerImplementation29CheckIncentivizedAvailabilityE" + str).c_str(), "adslib.CheckIncentivizedAvailability", trace("adslib.CheckIncentivizedAvailability", 1));
        observe(("_ZN6adslib24AdsManagerImplementation16ShowIncentivizedE" + str).c_str(), "adslib.ShowIncentivized", trace("adslib.ShowIncentivized", 1));
        observe(("_ZN5GLAds28CheckIncentivisedAdAvailableE" + str0).c_str(), "GLAds.CheckIncentivisedAdAvailable", trace("GLAds.CheckIncentivisedAdAvailable", 1));
        observe(("_ZN18GLAdsStatusChecker19QueryIncentivisedAdE" + str0).c_str(), "GLAdsStatusChecker.QueryIncentivisedAd", trace("GLAdsStatusChecker.QueryIncentivisedAd", 1));
        observe("_ZN13OnlineManager15RequestAdRewardEv", "OnlineManager.RequestAdReward", trace("OnlineManager.RequestAdReward", -1));
        observe("_ZN14CTuningManager13OpenSpinWheelEv", "CTuningManager.OpenSpinWheel", trace("CTuningManager.OpenSpinWheel", -1));
    }
}
