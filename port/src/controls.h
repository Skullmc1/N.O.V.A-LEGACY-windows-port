// Keyboard and mouse controls, expressed as touches on the game's screen.
#pragma once
#include <SDL.h>

#include <functional>

class Controls {
public:
    // touch(action, x, y, finger): action 0 = down, 1 = move, 2 = up.
    using TouchFn = std::function<void(int, float, float, int)>;
    Controls(SDL_Window* window, TouchFn touch);
    ~Controls();
    // Returns true if the event was used. `typing` is true while a text field is open.
    bool handle(const SDL_Event& ev, bool typing);
    void tick();          // call every loop iteration

private:
    struct State;
    State* s;
};
