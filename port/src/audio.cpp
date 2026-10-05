// OpenSL ES, as far as the game's sound engine (vox) uses it: one engine, an
// output mix, and audio players fed through a buffer queue. PCM goes to SDL.
//
// OpenSL objects and interfaces are pointers to a pointer to a table of
// function pointers; `self` is the first argument of every method.
#include <SDL.h>

#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "libc.h"

namespace {
struct Player {
    SDL_AudioDeviceID dev = 0;
    int channels = 2, rate = 44100, bits = 16;
    std::mutex m;
    std::deque<std::pair<const u8*, u32>> pending;     // buffers the guest queued, not yet handed to SDL
    u32 play_index = 0;
    u64 callback = 0, context = 0, queue_itf = 0;
    bool playing = false, destroyed = false;
};

struct Itf {
    u64* table;          // must be first: the guest dereferences this
    std::string kind;
    Player* player;
};

std::mutex g_players_mutex;
std::vector<Player*> g_players;
bool g_pump_started = false;

u64 make_itf(const std::string& kind, Player* player);

u32 frame_bytes(const Player* p) { return static_cast<u32>(p->channels * p->bits / 8); }

// Feeds SDL from the guest's queue and tells the guest when a buffer is done,
// which is its cue to mix and enqueue the next one.
void pump() {
    Thread guest_thread("audio-pump", 1 << 20, 48);
    guest_thread.make_current();
    for (;;) {
        std::vector<Player*> players;
        {
            std::lock_guard lk(g_players_mutex);
            players = g_players;
        }
        for (Player* p : players) {
            if (p->destroyed || !p->playing || !p->dev) continue;
            u32 target = static_cast<u32>(p->rate * frame_bytes(p) / 12);     // keep ~80 ms buffered
            for (int guard = 0; guard < 32 && SDL_GetQueuedAudioSize(p->dev) < target; guard++) {
                std::pair<const u8*, u32> buf{nullptr, 0};
                u64 cb, ctx, itf;
                {
                    std::lock_guard lk(p->m);
                    if (p->pending.empty()) break;
                    buf = p->pending.front();
                    p->pending.pop_front();
                    p->play_index++;
                    cb = p->callback; ctx = p->context; itf = p->queue_itf;
                }
                SDL_QueueAudio(p->dev, buf.first, buf.second);
                if (cb) guest_thread.call(cb, {itf, ctx});
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
}

void method(Thread& t, Itf* self, int idx) {
    Player* p = self->player;
    const std::string& kind = self->kind;
    u32 result = 0;
    if (kind == "engine_object" || kind == "mix_object" || kind == "player_object") {
        if (idx == 3) {                                         // GetInterface(self, iid, *out)
            const u8* iid = reinterpret_cast<const u8*>(t.x(1));
            const char* which = iid && *iid == 1 ? "engine" : iid && *iid == 2 ? "play" : iid && *iid == 3 ? "queue" : "other";
            u64 itf = make_itf(which, p);
            if (p && std::string(which) == "queue") p->queue_itf = itf;
            *reinterpret_cast<u64*>(t.x(2)) = itf;
        } else if (idx == 6 && p) {                             // Destroy
            p->destroyed = true;
            if (p->dev) SDL_CloseAudioDevice(p->dev);
            p->dev = 0;
        }
    } else if (kind == "engine") {
        if (idx == 7) {                                         // CreateOutputMix(self, *out, ...)
            *reinterpret_cast<u64*>(t.x(1)) = make_itf("mix_object", nullptr);
        } else if (idx == 2) {                                  // CreateAudioPlayer(self, *out, src, sink, ...)
            auto* np = new Player;
            const u64* src = reinterpret_cast<const u64*>(t.x(2));
            const u32* fmt = src ? reinterpret_cast<const u32*>(src[1]) : nullptr;
            if (fmt && fmt[0] == 2) {                           // SL_DATAFORMAT_PCM
                np->channels = static_cast<int>(fmt[1]);
                np->rate = static_cast<int>(fmt[2] / 1000);     // given in milliHertz
                np->bits = static_cast<int>(fmt[3]);
            }
            SDL_AudioSpec want{}, have{};
            want.freq = np->rate;
            want.channels = static_cast<Uint8>(np->channels);
            want.format = np->bits == 8 ? AUDIO_U8 : AUDIO_S16LSB;
            want.samples = 1024;
            np->dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
            logf("[audio] player: %d Hz, %d channel(s), %d-bit -> device %u%s", np->rate, np->channels, np->bits, np->dev,
                 np->dev ? "" : " (failed to open; silent)");
            {
                std::lock_guard lk(g_players_mutex);
                g_players.push_back(np);
                if (!g_pump_started) { g_pump_started = true; std::thread(pump).detach(); }
            }
            *reinterpret_cast<u64*>(t.x(1)) = make_itf("player_object", np);
        } else {
            log_once("sl:engine" + std::to_string(idx), "[audio] unhandled engine method #%d", idx);
        }
    } else if (kind == "play" && p) {
        if (idx == 0) {                                         // SetPlayState(self, state): 3 = playing
            p->playing = static_cast<u32>(t.x(1)) == 3;
            if (p->dev) SDL_PauseAudioDevice(p->dev, p->playing ? 0 : 1);
        } else if (idx == 1) {                                  // GetPlayState(self, *state)
            *reinterpret_cast<u32*>(t.x(1)) = p->playing ? 3 : 1;
        }
    } else if (kind == "queue" && p) {
        std::lock_guard lk(p->m);
        if (idx == 0) {                                         // Enqueue(self, buffer, size)
            p->pending.emplace_back(reinterpret_cast<const u8*>(t.x(1)), static_cast<u32>(t.x(2)));
        } else if (idx == 1) {                                  // Clear
            p->pending.clear();
            if (p->dev) SDL_ClearQueuedAudio(p->dev);
        } else if (idx == 2) {                                  // GetState(self, {count, playIndex}*)
            u32* st = reinterpret_cast<u32*>(t.x(1));
            st[0] = static_cast<u32>(p->pending.size());
            st[1] = p->play_index;
        } else if (idx == 3) {                                  // RegisterCallback(self, fn, context)
            p->callback = t.x(1);
            p->context = t.x(2);
        }
    }
    t.setx(0, result);
}

u64 make_itf(const std::string& kind, Player* player) {
    auto* itf = new Itf{new u64[32], kind, player};
    for (int i = 0; i < 32; i++)
        itf->table[i] = guest::make_stub("SL:" + kind + "#" + std::to_string(i),
                                         [i](Thread& t) { method(t, reinterpret_cast<Itf*>(t.x(0)), i); });
    return reinterpret_cast<u64>(itf);
}
}  // namespace

void register_audio() {
    // SL_IID_* are pointers to interface-id structs; we tag them 1/2/3.
    int tag = 1;
    for (const char* n : {"SL_IID_ENGINE", "SL_IID_PLAY", "SL_IID_BUFFERQUEUE"}) {
        guest::reg_data(n, 8, [tag](u8* p) {
            u8* id = new u8[16];
            memset(id, tag, 16);
            memcpy(p, &id, 8);
        });
        tag++;
    }
    IMPORT("slCreateEngine", [](u64* out) -> u32 { *out = make_itf("engine_object", nullptr); return 0; });
}
