// Keyboard and mouse -> the game's touch controls.
//
// The game only understands fingers on a 1280x720 touchscreen. In "game mode"
// (mouse captured, toggled with Tab) we hold virtual fingers down for it:
//   finger 1  movement stick on the left side, driven by WASD
//   finger 2  look drag on the right side, driven by mouse motion
//   finger 3+ on-screen buttons, pressed while a key or mouse button is held
// In "menu mode" the mouse is a single finger (finger 0), as before.
#include "controls.h"

#include <cmath>
#include <fstream>
#include <map>
#include <sstream>

#include "guest.h"

namespace {
struct Point { float x = 0, y = 0; };

// Positions on the game's 1280x720 screen. Written to controls.cfg on first run;
// edit that file to move a button.
std::map<std::string, Point> g_pos = {
    {"move_center", {210, 500}},     // where the left thumb rests
    {"look_center", {900, 380}},     // where the right thumb starts each drag
    {"fire", {1150, 560}},
    {"aim", {1030, 640}},
    {"jump", {1200, 450}},
    {"reload", {1100, 60}},
    {"grenade", {1220, 345}},
    {"weapon_prev", {988, 72}},
    {"weapon_next", {1220, 45}},
    {"item1", {1022, 208}},
    {"item2", {1115, 198}},
    {"item3", {1207, 188}},
    {"pause", {75, 45}},
};
float g_move_radius = 110, g_sensitivity = 1.0f;

const std::map<SDL_Keycode, const char*> g_keys = {
    {SDLK_SPACE, "jump"}, {SDLK_r, "reload"}, {SDLK_g, "grenade"}, {SDLK_q, "weapon_prev"}, {SDLK_e, "weapon_next"},
    {SDLK_1, "item1"}, {SDLK_2, "item2"}, {SDLK_3, "item3"}, {SDLK_p, "pause"},
};

std::string cfg_path() { return project_root() + "/controls.cfg"; }

void load_config() {
    std::ifstream in(cfg_path());
    if (!in) {
        std::ofstream out(cfg_path());
        out << "# N.O.V.A. Legacy port: where each control sits on the game's 1280x720 screen.\n"
               "# Format: name = x y. Delete this file to restore the defaults.\n"
               "sensitivity = " << g_sensitivity << "\nmove_radius = " << g_move_radius << "\n";
        for (auto& [name, p] : g_pos) out << name << " = " << p.x << " " << p.y << "\n";
        return;
    }
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string name = line.substr(0, eq);
        name.erase(name.find_last_not_of(" \t") + 1);
        std::istringstream vals(line.substr(eq + 1));
        float a = 0, b = 0;
        vals >> a >> b;
        if (name == "sensitivity") g_sensitivity = a;
        else if (name == "move_radius") g_move_radius = a;
        else if (g_pos.count(name)) g_pos[name] = {a, b};
    }
}
}  // namespace

struct Controls::State {
    TouchFn touch;
    SDL_Window* window = nullptr;
    bool captured = false;
    bool menu_finger_down = false;
    // movement
    bool keys[4] = {};                 // W A S D
    bool move_down = false;
    Point move_at;
    // look
    bool look_down = false;
    Point look_at;
    float pending_dx = 0, pending_dy = 0;
    Uint32 last_look_ms = 0, last_emit_ms = 0;
    // buttons: name -> finger id while held
    std::map<std::string, int> held;
    int next_finger = 3;

    void set_title() {
        SDL_SetWindowTitle(window, captured ? "N.O.V.A. Legacy  -  game controls (Tab: free the mouse)"
                                            : "N.O.V.A. Legacy  -  menu mouse (Tab: game controls)");
    }

    void press(const std::string& name) {
        if (held.count(name)) return;
        int id = next_finger++;
        if (next_finger > 9) next_finger = 3;
        held[name] = id;
        touch(0, g_pos[name].x, g_pos[name].y, id);
    }
    void release(const std::string& name) {
        auto it = held.find(name);
        if (it == held.end()) return;
        touch(2, g_pos[name].x, g_pos[name].y, it->second);
        held.erase(it);
    }

    void update_move() {
        float dx = static_cast<float>(keys[3]) - static_cast<float>(keys[1]);
        float dy = static_cast<float>(keys[2]) - static_cast<float>(keys[0]);
        Point c = g_pos["move_center"];
        if (dx == 0 && dy == 0) {
            if (move_down) { touch(2, move_at.x, move_at.y, 1); move_down = false; }
            return;
        }
        float len = std::sqrt(dx * dx + dy * dy);
        Point target{c.x + dx / len * g_move_radius, c.y + dy / len * g_move_radius};
        if (!move_down) { touch(0, c.x, c.y, 1); move_down = true; }
        move_at = target;
        touch(1, target.x, target.y, 1);
    }

    void release_everything() {
        for (bool& k : keys) k = false;
        update_move();
        if (look_down) { touch(2, look_at.x, look_at.y, 2); look_down = false; }
        while (!held.empty()) release(held.begin()->first);
        pending_dx = pending_dy = 0;
    }

    void set_captured(bool on) {
        if (on == captured) return;
        release_everything();
        captured = on;
        SDL_SetRelativeMouseMode(on ? SDL_TRUE : SDL_FALSE);
        set_title();
    }
};

Controls::Controls(SDL_Window* window, TouchFn touch) : s(new State) {
    s->touch = std::move(touch);
    s->window = window;
    load_config();
    s->set_title();
}

Controls::~Controls() { delete s; }

bool Controls::handle(const SDL_Event& ev, bool typing) {
    switch (ev.type) {
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        bool down = ev.type == SDL_KEYDOWN;
        SDL_Keycode key = ev.key.keysym.sym;
        if (key == SDLK_TAB) {
            if (down && !ev.key.repeat) s->set_captured(!s->captured);
            return true;
        }
        if (!s->captured || typing) return false;
        int wasd = key == SDLK_w ? 0 : key == SDLK_a ? 1 : key == SDLK_s ? 2 : key == SDLK_d ? 3 : -1;
        if (wasd >= 0) {
            if (s->keys[wasd] != down) { s->keys[wasd] = down; s->update_move(); }
            return true;
        }
        auto it = g_keys.find(key);
        if (it != g_keys.end()) {
            if (down && !ev.key.repeat) s->press(it->second);
            if (!down) s->release(it->second);
            return true;
        }
        return false;
    }
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        bool down = ev.type == SDL_MOUSEBUTTONDOWN;
        if (s->captured) {
            const char* name = ev.button.button == SDL_BUTTON_LEFT ? "fire" : ev.button.button == SDL_BUTTON_RIGHT ? "aim" : nullptr;
            if (name) { if (down) s->press(name); else s->release(name); }
            return true;
        }
        if (ev.button.button == SDL_BUTTON_LEFT) {
            s->menu_finger_down = down;
            s->touch(down ? 0 : 2, static_cast<float>(ev.button.x), static_cast<float>(ev.button.y), 0);
        }
        return true;
    }
    case SDL_MOUSEMOTION:
        if (s->captured) {
            s->pending_dx += static_cast<float>(ev.motion.xrel) * g_sensitivity;
            s->pending_dy += static_cast<float>(ev.motion.yrel) * g_sensitivity;
        } else if (s->menu_finger_down) {
            s->touch(1, static_cast<float>(ev.motion.x), static_cast<float>(ev.motion.y), 0);
        }
        return true;
    case SDL_MOUSEWHEEL:
        if (s->captured && ev.wheel.y != 0) {
            const char* name = ev.wheel.y > 0 ? "weapon_next" : "weapon_prev";
            s->press(name);
            s->release(name);
        }
        return true;
    case SDL_WINDOWEVENT:
        if (ev.window.event == SDL_WINDOWEVENT_FOCUS_LOST) s->set_captured(false);
        return false;
    }
    return false;
}

void Controls::tick() {
    if (!s->captured) return;
    Uint32 now = SDL_GetTicks();
    bool moved = s->pending_dx != 0 || s->pending_dy != 0;
    if (moved && now - s->last_emit_ms >= 8) {            // at most ~120 look updates per second
        Point c = g_pos["look_center"];
        if (!s->look_down) {
            s->touch(0, c.x, c.y, 2);
            s->look_at = c;
            s->look_down = true;
        }
        Point next{s->look_at.x + s->pending_dx, s->look_at.y + s->pending_dy};
        // Keep the finger on the right half of the screen: lift it and start a new drag when it runs out of room.
        if (next.x < 680 || next.x > 1250 || next.y < 110 || next.y > 690) {
            s->touch(2, s->look_at.x, s->look_at.y, 2);
            s->touch(0, c.x, c.y, 2);
            next = {c.x + s->pending_dx, c.y + s->pending_dy};
            next.x = std::fmin(std::fmax(next.x, 690.f), 1240.f);
            next.y = std::fmin(std::fmax(next.y, 120.f), 680.f);
        }
        s->look_at = next;
        s->touch(1, next.x, next.y, 2);
        s->pending_dx = s->pending_dy = 0;
        s->last_emit_ms = s->last_look_ms = now;
    } else if (s->look_down && !moved && now - s->last_look_ms > 250) {
        s->touch(2, s->look_at.x, s->look_at.y, 2);      // mouse at rest: lift the look finger
        s->look_down = false;
    }
}
