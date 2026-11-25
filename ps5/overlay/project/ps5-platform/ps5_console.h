// The console's own services, for an installed PS5 title: the launch splash and the
// controller. platform/ps5/ps5_console.cpp in a title; a payload has neither
// (ps5-spike/headless_vulkan.cpp answers for it).
#pragma once

#include <cstdint>
#include <string>

namespace ps5 {

// The shell keeps the launch background over the title's frames until this is called.
void hide_splash();

// Player 1's controller as the console reports it: the buttons (PAD_* below), the sticks
// 0..255 with 128 at rest and down/right positive, the triggers 0..255.
struct Pad {
    uint32_t buttons = 0;
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
};
enum : uint32_t {
    PAD_L3 = 0x000002, PAD_R3 = 0x000004, PAD_OPTIONS = 0x000008,
    PAD_UP = 0x000010, PAD_RIGHT = 0x000020, PAD_DOWN = 0x000040, PAD_LEFT = 0x000080,
    PAD_L1 = 0x000400, PAD_R1 = 0x000800,
    PAD_TRIANGLE = 0x001000, PAD_CIRCLE = 0x002000, PAD_CROSS = 0x004000, PAD_SQUARE = 0x008000,
    PAD_TOUCH_PAD = 0x100000,
};

// False while no controller is open or connected. Call from one thread at a time.
bool pad_read(Pad& pad);
// The two rumble motors, 0..255 each.
void pad_rumble(uint8_t large, uint8_t small);

// Sound: a thread asks `fill` for each block of 48 kHz 16-bit stereo frames and hands it to
// the console's main output. One output at a time; audio_stop returns once the thread has
// ended, so `user` may be freed after it.
using AudioFill = void (*)(int16_t* frames, int count, void* user);
bool audio_start(AudioFill fill, void* user);
void audio_stop();

// Times the console's basic operations (thread-local access, the heap, locks, clocks, sleeps)
// and logs them: about a second at start-up, while the port is being tuned.
void benchmark();

// Ends the title: the log is written first, then the process is ended without running the C++
// runtime's exit handlers (the game's threads, DXVK and the display are still live; waiting for
// them left the game open after its Quit).
[[noreturn]] void quit(int code);

// An on-screen keyboard for the game's name entry (the console's own text dialog is out of reach of
// a title): drawn by DXVK's overlay (the dirt2keyboard item) and driven by the pad.
// keyboard_run blocks its caller until the player confirms (true, `result` is the text) or cancels.
bool keyboard_run(const std::string& initial, unsigned max_chars, std::string& result);
// Every pad sample goes through here first; true while the keyboard has the pad (the game then
// sees a pad at rest).
bool keyboard_feed(const Pad& pad);
bool keyboard_active();
// What to draw: the name so far, then the four rows of keys as they are now (capitals or not),
// one line each; the highlighted key's row (4 is the row of commands) and column. 0 when closed.
int keyboard_state(char* out, int capacity, int* row, int* col);

}  // namespace ps5
