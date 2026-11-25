// Launch glue for the installed title. The console mounts the title folder as /app0:
//   /app0/Motorhead/  the PC game's files (Game, Cars, Data, Tracks, Movies, Music, ...), read only
//   /app0/data/       everything the port writes or configures, all in one folder: the game's profile
//                     (motorhead.cfg, Input/, scores), motorhead.log (the one log: game, presenter, controller/sound, file diagnostics),
//                     output (optional display height)
// The two never mix. If /app0/data cannot be written, /data/motorhead is used instead.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

#include <SDL3/SDL_hints.h>

#include <sys/stat.h>
#include <unistd.h>

extern "C" int __real_main(int argc, char **argv);
extern "C" int sceSystemServiceLoadExec(const char *path, char *const argv[]);
extern "C" void mh_ps5_log(const char *text);
extern "C" void mh_ps5_input_start(void);

namespace {
std::string g_data = "/app0/data";

bool writable(const std::string &dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string probe = dir + "/.write-test";
    if (FILE *f = std::fopen(probe.c_str(), "w")) {
        std::fclose(f);
        std::remove(probe.c_str());
        return true;
    }
    return false;
}
}  // namespace

// SDL's base path (patches/motorhead/0002): the game derives its content root from it.
extern "C" const char *mh_ps5_base_path(void) { return "/app0/Motorhead/"; }

// The port's data folder (no trailing slash); the presenter keeps its log there.
extern "C" const char *mh_ps5_data_dir(void) { return g_data.c_str(); }

// The title's entry point (app_crt.cpp calls "main", the link wraps it).
extern "C" int __wrap_main(int argc, char **argv) {
    if (!writable(g_data)) g_data = "/data/motorhead";
    std::error_code ec;
    std::filesystem::create_directories(g_data + "/Input", ec);
    // The game's user-data root is the data folder itself, so motorhead.log lands there.
    setenv("MOTORHEAD_USER_DATA", g_data.c_str(), 1);
    // SDL only uses its dummy drivers when asked: the console has no window system (frames go to
    // the display through ps5_present.cpp, sound through the audio driver patch).
    // Set as hints, not environment variables: SDL reads its own copy of the environment, which
    // the console's setenv() does not reach.
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    // data/renderer: "software" keeps the CPU renderer (the default is the Vulkan one).
    if (FILE *f = std::fopen((g_data + "/renderer").c_str(), "r")) {
        char word[32] = {};
        if (std::fscanf(f, "%31s", word) == 1) setenv("MOTORHEAD_RENDERER", word, 1);
        std::fclose(f);
    }
    // data/verbose (any content): full diagnostics in motorhead.log instead of errors only.
    if (std::filesystem::exists(g_data + "/verbose", ec)) setenv("MOTORHEAD_VERBOSE", "1", 1);
    setenv("MOTORHEAD_DEBUG_LOG", (g_data + "/motorhead.log").c_str(), 1);
    if (chdir("/app0/Motorhead") != 0) std::fprintf(stderr, "[ps5] chdir /app0/Motorhead failed\n");
    mh_ps5_input_start();
    const int code = __real_main(argc, argv);
    // Ending the process from inside makes the console report an error. Asking the system to close
    // the title is how a game quits: it ends the process itself.
    mh_ps5_log("[ps5-launch] game finished, asking the system to close the title");
    std::fflush(nullptr);
    usleep(500 * 1000);
    const int rc = sceSystemServiceLoadExec("exit", nullptr);
    std::fprintf(stderr, "[ps5] the system did not close the title: %d\n", rc);
    usleep(1500 * 1000);
    _exit(code);
}
