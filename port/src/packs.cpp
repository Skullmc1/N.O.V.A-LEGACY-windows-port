// Custom reward packs for the offline game.
//
// Gameloft's store sold bundles: named lists of rewards that the game's
// BundleManager applies to the save. The store is gone, so packs are defined in
// packs.cfg and handed out with a key press (F1..F8), through that same
// BundleManager code path.
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

#include "guest.h"
#include "platform.h"

namespace {
struct Reward { std::string name; int amount; };
struct Pack { std::string title; std::vector<Reward> rewards; };

std::vector<Pack> g_packs;
u64 g_bundle_manager = 0;        // the game's BundleManager instance, captured when it initialises

std::mutex g_tasks_mutex;
std::vector<std::function<void(Thread&)>> g_tasks;

const char* const DEFAULT_PACKS =
    "# N.O.V.A. Legacy port: reward packs, one per line, granted with F1..F8.\n"
    "# Format:  Title: ITEM amount, ITEM amount, ...\n"
    "# Items:\n"
    "#   weapon cards   HG / SMG / SG / SR / PG / RPG, then _TIER01_cards, _TIER02_cards or _TIER03_cards\n"
    "#                  (enough cards unlock the weapon and level it up)\n"
    "#   LEGENDARY_WEAPONS n   unlock the tier 4 weapon of every class at level n\n"
    "#   crates         GACHA_ELITE, GACHA_DIAMOND, GACHA_SPECIALOPS, GACHA_SOLDIER, SHADOW_PACK, RUSH_ELITE_PACK\n"
    "#   skins          any weap_... or char_... item name, or ALL_WEAPON_SKINS 1 / ALL_ARMOR_SKINS 1\n"
    "#   supplies       GR01 (grenades), BELT, ENERGYEXTENDER, coins, hard (trilithium)\n"
    "Arsenal: HG_TIER01_cards 300, HG_TIER02_cards 300, HG_TIER03_cards 300, SMG_TIER01_cards 300, SMG_TIER02_cards 300, "
    "SMG_TIER03_cards 300, SG_TIER01_cards 300, SG_TIER02_cards 300, SG_TIER03_cards 300, SR_TIER01_cards 300, "
    "SR_TIER02_cards 300, SR_TIER03_cards 300, PG_TIER01_cards 300, PG_TIER02_cards 300, PG_TIER03_cards 300, "
    "RPG_TIER01_cards 300, RPG_TIER02_cards 300, RPG_TIER03_cards 300\n"
    "Legendary Arsenal: LEGENDARY_WEAPONS 1\n"
    "Skin Vault: ALL_WEAPON_SKINS 1, ALL_ARMOR_SKINS 1\n"
    "Crate Haul: GACHA_ELITE 10, GACHA_DIAMOND 5, GACHA_SPECIALOPS 5\n"
    "Supplies: GR01 50, BELT 10, ENERGYEXTENDER 5, coins 250000, hard 50000\n";

#include "skin_names.inc"

// Shorthand items in packs.cfg that stand for many rewards.
void expand(const Reward& r, std::vector<Reward>& out) {
    if (r.name == "ALL_WEAPON_SKINS") { for (const char* n : WEAPON_SKINS) out.push_back({n, 1}); }
    else if (r.name == "ALL_ARMOR_SKINS") { for (const char* n : ARMOR_SKINS) out.push_back({n, 1}); }
    else out.push_back(r);
}

void load_packs() {
    std::string path = project_root() + "/packs.cfg";
    if (!std::filesystem::exists(path)) std::ofstream(path) << DEFAULT_PACKS;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        size_t colon = line.find(':');
        if (line.empty() || line[0] == '#' || colon == std::string::npos) continue;
        Pack pack;
        pack.title = line.substr(0, colon);
        std::istringstream items(line.substr(colon + 1));
        std::string item;
        while (std::getline(items, item, ',')) {
            std::istringstream one(item);
            Reward r;
            if (one >> r.name >> r.amount) expand(r, pack.rewards);
        }
        if (!pack.rewards.empty()) g_packs.push_back(pack);
    }
}

// A libc++ std::string as the guest lays it out (24 bytes).
struct GuestString {
    u64 words[3] = {};
    std::string backing;
    explicit GuestString(const std::string& s) : backing(s) {
        if (s.size() < 23) {
            u8* p = reinterpret_cast<u8*>(words);
            p[0] = static_cast<u8>(s.size() << 1);
            memcpy(p + 1, s.data(), s.size());
        } else {
            // Long form pointing at host memory. The guest only reads and copies it.
            words[0] = (s.size() + 1) | 1;
            words[1] = s.size();
            words[2] = reinterpret_cast<u64>(backing.c_str());
        }
    }
};

// The game's current save as JSON text.
std::string save_text(Thread& t) {
    static const u64 ctor = guest::sym("_ZN8GameSaveC1ENS_12GameSaveTypeE");
    static const u64 as_string = guest::sym("_ZNK8GameSave11GetAsStringEv");
    if (!ctor || !as_string) return "";
    static u8 save_object[512];
    memset(save_object, 0, sizeof save_object);
    t.call(ctor, {reinterpret_cast<u64>(save_object), 2});
    u64 out[3] = {};
    t.setx(8, reinterpret_cast<u64>(out));                 // the returned std::string is built here
    t.call(as_string, {reinterpret_cast<u64>(save_object)});
    const u8* p = reinterpret_cast<const u8*>(out);
    return (p[0] & 1) ? std::string(reinterpret_cast<const char*>(out[2]), out[1])
                      : std::string(reinterpret_cast<const char*>(p + 1), p[0] >> 1);
}

// Replace the game's save with the given JSON text, through its own parser and loader.
bool load_save_text(Thread& t, const std::string& text) {
    static const u64 reader_ctor = guest::sym("_ZN4Json6ReaderC1Ev");
    static const u64 value_ctor = guest::sym("_ZN4Json5ValueC1ENS_9ValueTypeE");
    static const u64 parse = guest::sym("_ZN4Json6Reader5parseERKNSt6__ndk112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEERNS_5ValueEb");
    static const u64 save_ctor = guest::sym("_ZN8GameSaveC1ENS_12GameSaveTypeE");
    static const u64 load = guest::sym("_ZN8GameSave4LoadEN4Json5ValueE");
    if (!reader_ctor || !value_ctor || !parse || !save_ctor || !load) return false;
    u8* reader = static_cast<u8*>(calloc(1, 4096));
    u8* root = static_cast<u8*>(calloc(1, 64));
    u8* save_object = static_cast<u8*>(calloc(1, 512));
    t.call(reader_ctor, {reinterpret_cast<u64>(reader)});
    t.call(value_ctor, {reinterpret_cast<u64>(root), 0});
    GuestString doc(text);
    bool ok = t.call(parse, {reinterpret_cast<u64>(reader), reinterpret_cast<u64>(doc.words), reinterpret_cast<u64>(root), 0}) & 1;
    if (ok) {
        t.call(save_ctor, {reinterpret_cast<u64>(save_object), 2});
        t.call(load, {reinterpret_cast<u64>(save_object), reinterpret_cast<u64>(root)});
    }
    return ok;       // the small helper objects are left allocated on purpose
}

// The bundle code only knows tiers 1 to 3, so the legendary tier is set in the save itself.
void unlock_legendary(Thread& t, int level) {
    std::string text = save_text(t);
    size_t classes = text.find("\"weapon_classes\"");
    if (classes == std::string::npos) { logf("[packs] legendary unlock failed: save has no weapon list"); return; }
    size_t end = text.find(']', classes);
    int changed = 0;
    const std::string locked = "\"tier4\":0";
    for (size_t at = text.find(locked, classes); at != std::string::npos && at < end; at = text.find(locked, at)) {
        std::string unlocked = "\"tier4\":" + std::to_string(level);
        text.replace(at, locked.size(), unlocked);
        end += unlocked.size() - locked.size();
        at += unlocked.size();
        changed++;
    }
    if (!changed) { logf("[packs] legendary weapons were already unlocked"); return; }
    logf("[packs] legendary tier set to level %d for %d weapon class(es): %s", level, changed,
         load_save_text(t, text) ? "applied" : "FAILED to parse the edited save");
}

void grant(Thread& t, const Pack& full_pack) {
    Pack pack;
    pack.title = full_pack.title;
    for (auto& r : full_pack.rewards) {
        if (r.name == "LEGENDARY_WEAPONS") unlock_legendary(t, std::max(r.amount, 1));
        else pack.rewards.push_back(r);
    }
    static const u64 prize_ctor = guest::sym("_ZN11OnlinePrizeC2ENSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEEi");
    static const u64 apply = guest::sym("_ZN13BundleManager18ApplyBundleRewardsERKNSt6__ndk16vectorI11OnlinePrizeNS0_9allocatorIS2_EEEE");
    if (!g_bundle_manager || !prize_ctor || !apply) {
        logf("[packs] cannot grant \"%s\": the game's bundle manager is not ready", pack.title.c_str());
        return;
    }
    static const u64 trigger_save_early = guest::sym("_Z23TriggerGenerateSavegamev");
    if (pack.rewards.empty()) {
        if (trigger_save_early) t.call(trigger_save_early);
        logf("[packs] granted \"%s\"", pack.title.c_str());
        return;
    }
    constexpr size_t PRIZE_SIZE = 0x38;             // sizeof(OnlinePrize) in this build
    size_t n = pack.rewards.size();
    u8* prizes = static_cast<u8*>(calloc(n, PRIZE_SIZE));
    for (size_t i = 0; i < n; i++) {
        GuestString name(pack.rewards[i].name);
        t.call(prize_ctor, {reinterpret_cast<u64>(prizes + i * PRIZE_SIZE), reinterpret_cast<u64>(name.words),
                            static_cast<u64>(pack.rewards[i].amount)});
    }
    if (getenv("NOVA_PACK_DEBUG")) {
        for (size_t i = 0; i < n; i++) {
            const u64* w = reinterpret_cast<const u64*>(prizes + i * PRIZE_SIZE);
            logf("[packs] prize %zu: %016llx %016llx %016llx %016llx | %016llx %016llx %016llx", i, w[0], w[1], w[2], w[3], w[4], w[5], w[6]);
        }
    }
    u64 vec[3] = {reinterpret_cast<u64>(prizes), reinterpret_cast<u64>(prizes + n * PRIZE_SIZE),
                  reinterpret_cast<u64>(prizes + n * PRIZE_SIZE)};
    t.call(apply, {reinterpret_cast<u64>(vec)});     // a static function: the list is its only argument
    // A real bundle purchase finishes by asking the game to write its save.
    static const u64 trigger_save = guest::sym("_Z23TriggerGenerateSavegamev");
    if (trigger_save) t.call(trigger_save);
    std::string list;
    for (auto& r : pack.rewards) list += r.name + " x" + std::to_string(r.amount) + "  ";
    logf("[packs] granted \"%s\": %s", pack.title.c_str(), list.c_str());
    // The prize objects are deliberately not freed: the game may keep references to their strings.
}
}  // namespace

void packs::init() {
    load_packs();
    // Keep a copy of the save file as it was when this session started.
    std::error_code ec;
    std::filesystem::copy_file(project_root() + "/work/fs/data/files/tdata", project_root() + "/work/fs/save-before-this-session.bak",
                               std::filesystem::copy_options::overwrite_existing, ec);
    logf("[packs] %zu pack(s) loaded from packs.cfg (F1..F%zu)", g_packs.size(), std::min<size_t>(g_packs.size(), 8));
}

void packs::set_bundle_manager(u64 instance) { g_bundle_manager = instance; }

// Debugging aid (F9): write the game's current save as readable JSON to work/save_dump.json.
static void dump_save(Thread& t) {
    std::string text = save_text(t);
    std::ofstream(project_root() + "/work/save_dump.json", std::ios::binary) << text;
    logf("[packs] wrote the save as JSON (%zu bytes) to work/save_dump.json", text.size());
}

void packs::request(int index) {
    if (index == 8) { platform::post_to_game_thread(dump_save); return; }      // F9
    if (index < 0 || index >= static_cast<int>(g_packs.size())) return;
    Pack pack = g_packs[index];
    platform::post_to_game_thread([pack](Thread& t) { grant(t, pack); });
}

// ---- running work on the game's own thread --------------------------------------------
void platform::post_to_game_thread(std::function<void(Thread&)> task) {
    std::lock_guard lk(g_tasks_mutex);
    g_tasks.push_back(std::move(task));
}

void platform::run_game_thread_tasks(Thread& t) {
    std::vector<std::function<void(Thread&)>> tasks;
    {
        std::lock_guard lk(g_tasks_mutex);
        tasks.swap(g_tasks);
    }
    for (auto& task : tasks) task(t);
}
