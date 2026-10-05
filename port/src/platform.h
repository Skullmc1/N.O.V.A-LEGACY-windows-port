// The host window the game renders into.
#pragma once
#include <functional>
#include <string>

struct Thread;

namespace platform {
void* window_token();     // what the guest sees as its ANativeWindow*
void* hwnd();             // the real Win32 window handle
int width();
int height();
int target_fps();              // frame-rate cap given to the game (--fps, default 60)
void* game_thread_handle();   // host thread running the game's main loop (for --profile)
void set_game_thread_handle(void* h);
// Run something on the game's main-loop thread at the end of the current frame.
void post_to_game_thread(std::function<void(Thread&)> task);
void run_game_thread_tasks(Thread& t);
}

namespace gfx {
extern bool frame_stats;    // --frame-stats: log frame rate and worst frame every 5 seconds
extern double shot_every;   // seconds between saved screenshots (0 = off)
}

// The on-screen keyboard the game asks Java for; we feed it from the real keyboard.
namespace keyboard {
bool visible();
void hide();
// Apply typed text / a backspace. Returns true and fills *out when the text changed.
bool on_text(const char* utf8, std::string* out);
bool on_backspace(std::string* out);
}

// Reward packs granted with F1..F8 (see packs.cfg).
namespace packs {
void init();
void set_bundle_manager(unsigned long long instance);
void request(int index);
}
