#include "app/Application.hpp"
#include <cstdlib>
#include <cstdio>
#include <memory>

int main(int argc, char** argv) {
    debrief::AppConfig cfg;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "--demo"))                         { cfg.demo_mode = true; }
        else if (!strcmp(a, "--port") && i+1<argc)        { cfg.udp_port = (uint16_t)atoi(argv[++i]); }
        else if (!strcmp(a, "--addr") && i+1<argc)        { cfg.bind_addr = argv[++i]; }
        else if (!strcmp(a, "--width") && i+1<argc)       { cfg.window_width  = atoi(argv[++i]); }
        else if (!strcmp(a, "--height") && i+1<argc)      { cfg.window_height = atoi(argv[++i]); }
        else if (!strcmp(a, "--help")) {
            puts("AfterAction — tactical telemetry viewer & debrief");
            puts("Usage: afteraction [options]");
            puts("  --demo            Run built-in flight demo (no UDP needed)");
            puts("  --port  N         UDP listen port (default 5555)");
            puts("  --addr  IP        Bind address (default 0.0.0.0)");
            puts("  --width  N        Window width  (default 1600)");
            puts("  --height N        Window height (default 900)");
            puts("\nKeyboard shortcuts:");
            puts("  Space      Play / Pause");
            puts("  F          Follow selected entity");
            puts("  R          Toggle ribbon trails");
            puts("  1/2/3/4    Playback speed: 0.25x / 1x / 4x / -1x");
            puts("  Esc        Quit");
            return 0;
        }
    }

    if (cfg.demo_mode)
        puts("[afteraction] Demo mode — no UDP required");
    else
        printf("[afteraction] Listening UDP on %s:%u\n", cfg.bind_addr.c_str(), cfg.udp_port);

    // Heap-allocate: Application embeds large fixed buffers (the TelemetryStore
    // ring is ~0.5 MB, plus the inbound queue). Keeping it off the stack leaves
    // the full stack available for the deep render/UI call path.
    auto app = std::make_unique<debrief::Application>(cfg);
    app->run();
    return 0;
}
