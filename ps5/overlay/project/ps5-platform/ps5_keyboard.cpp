// The on-screen keyboard (the console has none for a game that only knows keyboard text events).
// The front end opens it when X is pressed on a text row (frontend.cpp); this file keeps its state,
// reads the pad events while it is open, and draws it over the finished frame in the presenter
// (mh_ps5_present calls mh_ps5_kb_draw). When it closes with OK the front end types the result.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

constexpr const char *kLower[4] = {"1234567890", "qwertyuiop", "asdfghjkl_", "zxcvbnm.-!"};
constexpr const char *kUpper[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL_", "ZXCVBNM.-!"};
constexpr int kRows = 4, kCols = 10, kCommands = 5;  // SHIFT, SPACE, DEL, OK, CANCEL
constexpr const char *kCommandNames[kCommands] = {"SHIFT", "SPACE", "DEL", "OK", "CANCEL"};

struct Keyboard {
    bool active = false, upper = true, done = false, confirmed = false;
    std::string text;
    size_t max = 20;
    int row = 0, col = 0;
    int held_x = 0, held_y = 0;  // stick directions already acted on
} g;

SDL_Surface *g_surface = nullptr;
SDL_Renderer *g_renderer = nullptr;
void *g_pixels = nullptr;
int g_w = 0, g_h = 0, g_pitch = 0;

int row_width(int row) { return row < kRows ? kCols : kCommands; }

void move(int dx, int dy) {
    const int rows = kRows + 1;
    if (dy) {
        const int from = g.row;
        g.row = (g.row + dy + rows) % rows;
        if (g.row == kRows) g.col = g.col * kCommands / kCols;
        else if (from == kRows) g.col = std::min(kCols - 1, g.col * kCols / kCommands + 1);
    }
    if (dx) g.col = (g.col + dx + row_width(g.row)) % row_width(g.row);
}

void type(char c) {
    if (g.text.size() >= g.max) return;
    const bool first = g.text.empty();
    g.text += c;
    if (first && g.upper) g.upper = false;  // a name starts with a capital, then goes on in lower case
}

void erase() {
    if (!g.text.empty()) g.text.pop_back();
}

void press() {
    if (g.row < kRows) {
        type((g.upper ? kUpper : kLower)[g.row][g.col]);
        return;
    }
    switch (g.col) {
        case 0: g.upper = !g.upper; break;
        case 1: type(' '); break;
        case 2: erase(); break;
        case 3: g.done = g.confirmed = true; break;
        default: g.done = true; break;
    }
}

}  // namespace

extern "C" bool mh_ps5_kb_active(void) { return g.active; }

extern "C" void mh_ps5_kb_open(const char *initial, unsigned max_chars) {
    g.text = initial ? initial : "";
    g.max = max_chars ? max_chars : 20;
    if (g.text.size() > g.max) g.text.resize(g.max);
    g.row = g.col = 0;
    g.upper = g.text.empty();
    g.done = g.confirmed = false;
    g.held_x = g.held_y = 0;
    g.active = true;
}

// Feeds one SDL event. 0: the keyboard is closed or the event is not a pad event (the game handles
// it); 1: taken; 2: closed with OK, the text is in `out`; 3: closed without it.
extern "C" int mh_ps5_kb_event(const SDL_Event *event, char *out, int capacity) {
    if (!g.active) return 0;
    const auto type_of = event->type;
    if (type_of == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        const int value = event->gaxis.value;
        const int dir = value < -16000 ? -1 : value > 16000 ? 1 : 0;
        if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX) {
            if (dir != g.held_x && dir) move(dir, 0);
            g.held_x = dir;
        } else if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY) {
            if (dir != g.held_y && dir) move(0, dir);
            g.held_y = dir;
        }
        return 1;
    }
    if (type_of == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        switch (event->gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP: move(0, -1); break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN: move(0, 1); break;
            case SDL_GAMEPAD_BUTTON_DPAD_LEFT: move(-1, 0); break;
            case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: move(1, 0); break;
            case SDL_GAMEPAD_BUTTON_SOUTH: press(); break;
            case SDL_GAMEPAD_BUTTON_WEST: erase(); break;
            case SDL_GAMEPAD_BUTTON_NORTH: type(' '); break;
            case SDL_GAMEPAD_BUTTON_EAST: g.done = true; break;
            case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: g.upper = !g.upper; break;
            case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            case SDL_GAMEPAD_BUTTON_START: g.done = g.confirmed = true; break;
            default: break;
        }
        if (g.done) {
            g.active = false;
            if (g.confirmed && out && capacity > 0) {
                std::snprintf(out, size_t(capacity), "%s", g.text.c_str());
                return 2;
            }
            return 3;
        }
        return 1;
    }
    // Everything else from the pad is swallowed while the keyboard is up; window and quit events pass.
    return (type_of >= SDL_EVENT_JOYSTICK_AXIS_MOTION && type_of < SDL_EVENT_FINGER_DOWN) ? 1 : 0;
}

// Draws the keyboard onto the finished frame (XRGB8888) with SDL's software renderer.
extern "C" void mh_ps5_kb_draw(void *pixels, int w, int h, int pitch) {
    if (!g.active) return;
    if (!g_renderer || pixels != g_pixels || w != g_w || h != g_h || pitch != g_pitch) {
        if (g_renderer) SDL_DestroyRenderer(g_renderer);
        if (g_surface) SDL_DestroySurface(g_surface);
        g_surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_XRGB8888, pixels, pitch);
        g_renderer = g_surface ? SDL_CreateSoftwareRenderer(g_surface) : nullptr;
        g_pixels = pixels;
        g_w = w;
        g_h = h;
        g_pitch = pitch;
        if (!g_renderer) return;
        SDL_SetRenderDrawBlendMode(g_renderer, SDL_BLENDMODE_BLEND);
    }
    SDL_Renderer *r = g_renderer;
    const int sc = h >= 900 ? 3 : (h >= 540 ? 2 : 1);  // text scale: 8 pixel glyphs times this
    const float glyph = 8.0F * float(sc);
    const float cw = glyph * 2.4F, ch = glyph * 1.9F;
    const float panel_w = cw * kCols + glyph * 2, panel_h = ch * (kRows + 6.2F) + glyph;
    const float x0 = (float(w) - panel_w) / 2, y0 = (float(h) - panel_h) / 2;

    const auto fill = [&](float x, float y, float fw, float fh, Uint8 R, Uint8 G, Uint8 B, Uint8 A) {
        SDL_SetRenderScale(r, 1.0F, 1.0F);
        SDL_SetRenderDrawColor(r, R, G, B, A);
        const SDL_FRect rect{x, y, fw, fh};
        SDL_RenderFillRect(r, &rect);
    };
    const auto text = [&](float x, float y, const std::string &s, float scale, Uint8 R, Uint8 G, Uint8 B) {
        SDL_SetRenderScale(r, scale, scale);
        SDL_SetRenderDrawColor(r, R, G, B, 255);
        SDL_RenderDebugText(r, x / scale, y / scale, s.c_str());
    };

    fill(0, 0, float(w), float(h), 0, 0, 0, 150);                             // dims the menu
    fill(x0 - 4, y0 - 4, panel_w + 8, panel_h + 8, 255, 100, 0, 255);         // border
    fill(x0, y0, panel_w, panel_h, 8, 8, 56, 255);
    text(x0 + glyph, y0 + glyph * 0.6F, "TYPE THE TEXT", float(sc), 255, 224, 96);
    fill(x0 + glyph, y0 + ch + glyph * 0.2F, panel_w - glyph * 2, ch * 0.95F, 0, 0, 24, 255);
    text(x0 + glyph * 1.5F, y0 + ch + glyph * 0.6F, g.text + "_", float(sc), 255, 255, 255);

    const float grid_y = y0 + ch * 2.4F;
    for (int row = 0; row < kRows; ++row)
        for (int col = 0; col < kCols; ++col) {
            const bool sel = g.row == row && g.col == col;
            const float cx = x0 + glyph + col * cw, cy = grid_y + row * ch;
            if (sel) fill(cx + 2, cy + 2, cw - 4, ch - 4, 255, 224, 96, 255);
            else fill(cx + 2, cy + 2, cw - 4, ch - 4, 24, 24, 96, 255);
            const char c[2] = {(g.upper ? kUpper : kLower)[row][col], 0};
            text(cx + (cw - glyph) / 2, cy + (ch - glyph) / 2, c, float(sc), sel ? 0 : 255, sel ? 0 : 255, sel ? 0 : 255);
        }
    const float cmd_y = grid_y + kRows * ch + ch * 0.2F;
    const float cmd_w = (cw * kCols) / kCommands;
    for (int i = 0; i < kCommands; ++i) {
        const bool sel = g.row == kRows && g.col == i;
        const float cx = x0 + glyph + i * cmd_w;
        if (sel) fill(cx + 2, cmd_y + 2, cmd_w - 4, ch - 4, 255, 224, 96, 255);
        else fill(cx + 2, cmd_y + 2, cmd_w - 4, ch - 4, 96, 40, 8, 255);
        const std::string name = kCommandNames[i];
        const float tw = glyph * float(name.size()) * (sc > 1 ? 0.67F : 1.0F);
        text(cx + (cmd_w - tw) / 2, cmd_y + (ch - glyph * 0.67F) / 2, name, sc > 1 ? float(sc) * 0.67F : 1.0F,
             sel ? 0 : 255, sel ? 0 : 255, sel ? 0 : 255);
    }
    const float small = float(std::max(1, sc - 1));
    text(x0 + glyph, cmd_y + ch * 1.35F, "X SELECT  SQUARE DELETE  TRIANGLE SPACE", small, 200, 200, 255);
    text(x0 + glyph, cmd_y + ch * 1.35F + 8 * small * 1.4F, "L1 SHIFT  R1 OK  CIRCLE CANCEL", small, 200, 200, 255);
    SDL_FlushRenderer(r);
}
