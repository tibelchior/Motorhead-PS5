// The console's services for the installed title: the launch splash and the controller.
// No SDK header declares these calls; the declarations and the pad sample layout are the
// ones mihawk-99/PS5_VulkanTemplate (ps5/src/platform.c) runs with.
#include "ps5_console.h"

#include <chrono>
#include <algorithm>
#include <cstddef>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <pthread.h>
#include <stdio.h>
#include <sched.h>
#include <unistd.h>

#include "recomp/host.h"

extern "C" {
int sceSystemServiceHideSplashScreen(void);
int sceSystemServiceLoadExec(const char* path, char* const argv[]);
int sceUserServiceInitialize(const void* params);
int sceUserServiceGetInitialUser(int32_t* user_id);
int scePadInit(void);
int scePadOpen(int32_t user_id, int32_t port_type, int32_t index, const void* params);
int scePadGetHandle(int32_t user_id, int32_t port_type, int32_t index);
int scePadRead(int32_t handle, void* samples, int32_t capacity);
int scePadSetVibrationMode(int32_t handle, int32_t mode);
int scePadSetVibration(int32_t handle, const void* vibration);
int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t grain, uint32_t rate, uint32_t format);
int sceAudioOutOutput(int32_t port, const void* samples);
int sceAudioOutClose(int32_t port);
int sceKernelUsleep(uint32_t microseconds);
}

namespace ps5 {

void hide_splash() { rt::log("ps5: splash dismissed: %d", sceSystemServiceHideSplashScreen()); }

namespace {

// One sample as the console writes it.
struct Sample {
    uint32_t buttons;
    uint8_t left_x, left_y, right_x, right_y, l2, r2;
    uint8_t reserved[66];
    int32_t connected;
    uint64_t timestamp_us;
    uint8_t extension[16];
    uint8_t connected_count;
    uint8_t remaining[15];
};
static_assert(sizeof(Sample) == 120, "the console's pad samples are 120 bytes");
static_assert(offsetof(Sample, connected) == 0x4c, "connection state sits at 0x4c");

constexpr uint32_t kIntercepted = 0x80000000u;  // the shell has the controller (PS button menus)
constexpr int kRumbleCompatible = 2;            // the two-motor rumble, not the DualSense haptics

int32_t g_handle = -1;
bool g_service = false, g_service_tried = false, g_connected = false;
time_t g_next_open = 0;
Pad g_last;

// The pad of the user who started the title. The pad service can publish the controller
// after the title starts, so a failed open is tried again once a second.
void open_pad() {
    if (!g_service_tried) {
        g_service_tried = true;
        (void)sceUserServiceInitialize(nullptr);
        const int rc = scePadInit();
        g_service = rc >= 0;
        if (!g_service) rt::log("ps5: scePadInit failed: 0x%08x", unsigned(rc));
    }
    if (!g_service) return;
    const time_t now = time(nullptr);
    if (now < g_next_open) return;
    g_next_open = now + 1;
    int32_t user = -1;
    if (sceUserServiceGetInitialUser(&user) < 0) return;
    int32_t handle = scePadOpen(user, 0, 0, nullptr);
    if (handle < 0) handle = scePadGetHandle(user, 0, 0);  // one this process already holds
    if (handle < 0) return;
    g_handle = handle;
    rt::log("ps5: controller opened for user %d, rumble mode %d", int(user), scePadSetVibrationMode(handle, kRumbleCompatible));
}

}  // namespace

bool pad_read(Pad& pad) {
    if (g_handle < 0) open_pad();
    if (g_handle < 0) return false;
    static Sample samples[16];
    const int count = scePadRead(g_handle, samples, 16);
    if (count < 0) g_connected = false;
    for (int i = 0; i < count && i < 16; ++i) {
        const Sample& s = samples[i];
        g_connected = s.connected != 0;
        if (s.connected && !(s.buttons & kIntercepted))
            g_last = Pad{s.buttons, s.left_x, s.left_y, s.right_x, s.right_y, s.l2, s.r2};
    }
    if (!g_connected) g_last = Pad{};
    pad = g_last;
    return g_connected;
}

void pad_rumble(uint8_t large, uint8_t small) {
    if (g_handle < 0) return;
    const uint8_t motors[2] = {large, small};
    (void)scePadSetVibration(g_handle, motors);
}

namespace {

constexpr int kGrain = 256;  // frames per block
constexpr int kAudioAlreadyInitialised = int(0x8026000e);

// The output thread is started once and never ends: audio_stop only takes its source away,
// and it plays silence until the next audio_start. A thread that has mixed OpenAL Soft's
// audio crashed in its thread-local destructors when it exited (a free of a pointer that
// was not a heap block, at the game's first alcCloseDevice), so it is not allowed to.
int32_t g_port = -1;
pthread_t g_audio_thread;
pthread_mutex_t g_fill_mu = PTHREAD_MUTEX_INITIALIZER;  // held while a block is filled
AudioFill g_fill = nullptr;
void* g_fill_user = nullptr;

void* audio_main(void*) {
    alignas(64) static int16_t block[kGrain * 2];
    unsigned errors = 0;
    for (;;) {
        pthread_mutex_lock(&g_fill_mu);
        if (g_fill) g_fill(block, kGrain, g_fill_user);
        else std::memset(block, 0, sizeof block);
        pthread_mutex_unlock(&g_fill_mu);
        // Blocks for one block's time, which paces the thread. A failing output is said once.
        if (sceAudioOutOutput(g_port, block) < 0) {
            if (errors++ == 0) rt::log("ps5: sceAudioOutOutput failed");
            sceKernelUsleep(5000);
        }
    }
    return nullptr;
}

}  // namespace

bool audio_start(AudioFill fill, void* user) {
    if (!fill) return false;
    if (g_port < 0) {
        const int init = sceAudioOutInit();
        if (init < 0 && init != kAudioAlreadyInitialised) {
            rt::log("ps5: sceAudioOutInit failed: 0x%08x", unsigned(init));
            return false;
        }
        // The system user's main port: 256 frames, 48 kHz, 16-bit stereo.
        const int32_t port = sceAudioOutOpen(0xff, 0, 0, kGrain, 48000, 1);
        if (port < 0) {
            rt::log("ps5: sceAudioOutOpen failed: 0x%08x", unsigned(port));
            return false;
        }
        g_port = port;
        // OpenAL Soft mixes on this thread, with its buffers on the stack.
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        rt::set_host_stack(&attr, 4u << 20);
        if (pthread_create(&g_audio_thread, &attr, audio_main, nullptr) != 0) {
            rt::log("ps5: no thread for the sound output");
            (void)sceAudioOutClose(g_port);
            g_port = -1;
            return false;
        }
        rt::log("ps5: sound output at 48 kHz stereo on port %d", int(g_port));
    }
    pthread_mutex_lock(&g_fill_mu);
    g_fill = fill;
    g_fill_user = user;
    pthread_mutex_unlock(&g_fill_mu);
    return true;
}

// Returns once no block is being filled, so `user` may be freed.
void audio_stop() {
    pthread_mutex_lock(&g_fill_mu);
    g_fill = nullptr;
    g_fill_user = nullptr;
    pthread_mutex_unlock(&g_fill_mu);
}

namespace {

// ---- the on-screen keyboard (QWERTY; drawn by DXVK's overlay, patches/dxvk/0008)
constexpr const char* kLowerRows[4] = {"1234567890", "qwertyuiop", "asdfghjkl_", "zxcvbnm.-!"};
constexpr const char* kUpperRows[4] = {"1234567890", "QWERTYUIOP", "ASDFGHJKL_", "ZXCVBNM.-!"};
constexpr int kKeyRows = 4, kKeyCols = 10;
constexpr int kCommands = 5;  // SHIFT, SPACE, DELETE, OK, CANCEL: the row below the keys

struct Keyboard {
    std::mutex mu;
    bool active = false, done = false, confirmed = false, upper = true;
    std::string text;
    size_t max = 14;
    int row = 0, col = 0;
    uint32_t prev = ~0u;  // buttons held when it opened do not count until released
    bool l2 = true, r2 = true;
    bool swallow = false;  // after it closes the game gets no input until every button is released
    int held = 0;         // direction held: 1 up, 2 down, 3 left, 4 right
    std::chrono::steady_clock::time_point next_repeat;
} g_kb;

int row_width(int row) { return row < kKeyRows ? kKeyCols : kCommands; }

void kb_move(int dx, int dy) {
    Keyboard& k = g_kb;
    const int rows = kKeyRows + 1;
    if (dy) {
        const int from = k.row;
        k.row = (k.row + dy + rows) % rows;
        if (k.row == kKeyRows) k.col = k.col * kCommands / kKeyCols;                  // onto the commands
        else if (from == kKeyRows) k.col = std::min(kKeyCols - 1, k.col * kKeyCols / kCommands + 1);
    }
    if (dx) k.col = (k.col + dx + row_width(k.row)) % row_width(k.row);
}

void kb_type(char c) {
    Keyboard& k = g_kb;
    if (k.text.size() >= k.max) return;
    const bool first = k.text.empty();
    k.text += c;
    if (first && k.upper) k.upper = false;  // a name starts with a capital, then goes on in lower case
}

void kb_delete() {
    if (!g_kb.text.empty()) g_kb.text.pop_back();
}

void kb_press() {
    Keyboard& k = g_kb;
    if (k.row < kKeyRows) {
        kb_type((k.upper ? kUpperRows : kLowerRows)[k.row][k.col]);
        return;
    }
    switch (k.col) {
        case 0: k.upper = !k.upper; break;
        case 1: kb_type(' '); break;
        case 2: kb_delete(); break;
        case 3: k.done = k.confirmed = true; break;
        default: k.done = true; break;
    }
}

}  // namespace

bool keyboard_feed(const Pad& pad) {
    Keyboard& k = g_kb;
    std::lock_guard lk(k.mu);
    if (!k.active) {
        // The press that closed it (Cross on CANCEL, R2 for OK) is still held: the game would take it
        // as a press of its own and open the keyboard again.
        if (k.swallow && (pad.buttons & ~uint32_t(PAD_TOUCH_PAD)) == 0 && pad.l2 < 32 && pad.r2 < 32) k.swallow = false;
        return k.swallow;
    }
    const uint32_t b = pad.buttons;
    const uint32_t edge = b & ~k.prev;
    k.prev = b;
    int dir = (b & PAD_UP) ? 1 : (b & PAD_DOWN) ? 2 : (b & PAD_LEFT) ? 3 : (b & PAD_RIGHT) ? 4 : 0;
    if (!dir) dir = pad.ly < 56 ? 1 : pad.ly > 200 ? 2 : pad.lx < 56 ? 3 : pad.lx > 200 ? 4 : 0;
    const auto now = std::chrono::steady_clock::now();
    if (dir && (dir != k.held || now >= k.next_repeat)) {
        kb_move(dir == 4 ? 1 : dir == 3 ? -1 : 0, dir == 2 ? 1 : dir == 1 ? -1 : 0);
        k.next_repeat = now + std::chrono::milliseconds(dir != k.held ? 350 : 90);
    }
    k.held = dir;
    // The triggers are analog: pressed past halfway, once per press.
    const bool l2 = pad.l2 > 128, r2 = pad.r2 > 128;
    if (l2 && !k.l2) k.upper = !k.upper;
    if (r2 && !k.r2) k.done = k.confirmed = true;
    k.l2 = l2;
    k.r2 = r2;
    if (edge & PAD_CROSS) kb_press();
    if (edge & PAD_TRIANGLE) kb_type(' ');
    if (edge & PAD_SQUARE) kb_delete();
    if (edge & PAD_CIRCLE) k.done = true;
    return true;
}

bool keyboard_active() {
    std::lock_guard lk(g_kb.mu);
    return g_kb.active;
}

int keyboard_state(char* out, int capacity, int* row, int* col) {
    Keyboard& k = g_kb;
    std::lock_guard lk(k.mu);
    if (!k.active || capacity < 2) return 0;
    std::string t = k.text;
    for (int r = 0; r < kKeyRows; ++r) {
        t += '\n';
        t += (k.upper ? kUpperRows : kLowerRows)[r];
    }
    const int n = int(std::min<size_t>(t.size(), size_t(capacity - 1)));
    std::memcpy(out, t.data(), size_t(n));
    out[n] = 0;
    *row = k.row;
    *col = k.col;
    return n + 1;  // never 0 while open, even with an empty name
}

bool keyboard_run(const std::string& initial, unsigned max_chars, std::string& result) {
    Keyboard& k = g_kb;
    {
        std::lock_guard lk(k.mu);
        k.text = initial.substr(0, max_chars);
        k.max = max_chars;
        k.row = k.col = 0;
        k.upper = true;
        k.done = k.confirmed = false;
        k.prev = ~0u;
        k.l2 = k.r2 = true;  // a trigger held when it opened does not count
        k.held = 0;
        k.active = true;
    }
    for (;;) {
        usleep(20000);
        std::lock_guard lk(k.mu);
        if (k.done) {
            k.active = false;
            k.swallow = true;
            if (k.confirmed) result = k.text;
            return k.confirmed;
        }
    }
}

namespace {

thread_local int t_bench = 0;
__attribute__((noinline)) int bench_tls() { return ++t_bench; }

double bench_ns(const timespec& a, const timespec& b) {
    return double(b.tv_sec - a.tv_sec) * 1e9 + double(b.tv_nsec - a.tv_nsec);
}

template <typename F>
double per_op(int count, F&& f) {
    timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < count; ++i) f();
    clock_gettime(CLOCK_MONOTONIC, &b);
    return bench_ns(a, b) / count;
}

pthread_mutex_t g_bench_mu = PTHREAD_MUTEX_INITIALIZER;
void* bench_heap_thread(void*) {
    void* held[16] = {};
    for (int i = 0; i < 200000; ++i) {
        void*& p = held[i & 15];
        std::free(p);
        p = std::malloc(48 + (i & 63) * 8);
    }
    for (void* p : held) std::free(p);
    return nullptr;
}
void* bench_lock_thread(void*) {
    for (int i = 0; i < 200000; ++i) {
        pthread_mutex_lock(&g_bench_mu);
        pthread_mutex_unlock(&g_bench_mu);
    }
    return nullptr;
}
double four_threads_ms(void* (*body)(void*)) {
    timespec a, b;
    pthread_t threads[4];
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (pthread_t& t : threads) pthread_create(&t, nullptr, body, nullptr);
    for (pthread_t& t : threads) pthread_join(t, nullptr);
    clock_gettime(CLOCK_MONOTONIC, &b);
    return bench_ns(a, b) / 1e6;
}

}  // namespace

void benchmark() {
    static pthread_key_t key;
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_key_create(&key, nullptr);
    pthread_once(&once, [] {});
    volatile uintptr_t sink = 0;
    timespec ts;
    rt::log("bench: ns per call: pthread_getspecific %.0f, pthread_once %.0f, thread_local %.0f, malloc+free %.0f, "
            "mutex lock+unlock %.0f, clock_gettime %.0f, sched_yield %.0f",
            per_op(1000000, [&] { sink = sink + reinterpret_cast<uintptr_t>(pthread_getspecific(key)); }),
            per_op(1000000, [&] { pthread_once(&once, [] {}); }),
            per_op(1000000, [&] { sink = sink + uintptr_t(bench_tls()); }),
            per_op(1000000, [&] { void* p = std::malloc(64); sink = sink + reinterpret_cast<uintptr_t>(p); std::free(p); }),
            per_op(1000000, [&] { pthread_mutex_lock(&g_bench_mu); pthread_mutex_unlock(&g_bench_mu); }),
            per_op(200000, [&] { clock_gettime(CLOCK_MONOTONIC, &ts); }),
            per_op(20000, [&] { sched_yield(); }));
    rt::log("bench: a sleep of 50 us takes %.0f us, of 1 ms takes %.0f us",
            per_op(200, [] { usleep(50); }) / 1e3, per_op(100, [] { usleep(1000); }) / 1e3);
    rt::log("bench: four threads, 200000 each: malloc+free %.0f ms, one shared mutex %.0f ms",
            four_threads_ms(bench_heap_thread), four_threads_ms(bench_lock_thread));
}

void quit(int code) {
    rt::log("quitting (%d)", code);  // the log writer flushes on this line
    std::fflush(nullptr);
    usleep(500 * 1000);              // and gets a moment to write it
    // Ending the process from inside makes the console report an error. Asking the system to
    // close the title is how a game quits: it ends the process itself.
    const int rc = sceSystemServiceLoadExec("exit", nullptr);
    rt::log("quitting (the system did not close the title: %d)", rc);
    usleep(1500 * 1000);
    _exit(code);
}

}  // namespace ps5
