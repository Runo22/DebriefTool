#include "app/Application.hpp"
#include <raylib.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <ctime>
#include <exception>
#include <filesystem>
#include <memory>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
//  Logging + crash safety
//
//  Release builds have no console window (see CMake WIN32_EXECUTABLE), so all
//  raylib TraceLog output is mirrored to <exe_dir>/logs/AfterAction.log. On a
//  crash (signal or uncaught exception) we flush + close that log so the tail of
//  it points at what went wrong. In Debug we ALSO print to stderr (console).
// ─────────────────────────────────────────────────────────────────────────────
namespace {

FILE* g_log = nullptr;

void timestamp(char* buf, size_t n) {
    time_t t = time(nullptr);
    tm* lt = localtime(&t);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", lt);
}

void trace_log_to_file(int level, const char* text, va_list args) {
    const char* lvl = "INFO";
    switch (level) {
        case LOG_TRACE:   lvl = "TRACE";   break;
        case LOG_DEBUG:   lvl = "DEBUG";   break;
        case LOG_INFO:    lvl = "INFO";    break;
        case LOG_WARNING: lvl = "WARN";    break;
        case LOG_ERROR:   lvl = "ERROR";   break;
        case LOG_FATAL:   lvl = "FATAL";   break;
        default: break;
    }
    if (g_log) {
        char ts[32]; timestamp(ts, sizeof(ts));
        fprintf(g_log, "%s [%-5s] ", ts, lvl);
        va_list cp; va_copy(cp, args);
        vfprintf(g_log, text, cp);
        va_end(cp);
        fputc('\n', g_log);
        fflush(g_log);   // flush every line so a crash keeps the tail
    }
#ifndef NDEBUG
    va_list cp2; va_copy(cp2, args);
    fprintf(stderr, "[%-5s] ", lvl);
    vfprintf(stderr, text, cp2);
    va_end(cp2);
    fputc('\n', stderr);
#endif
}

void close_log() { if (g_log) { fclose(g_log); g_log = nullptr; } }

void crash_handler(int sig) {
    if (g_log) {
        char ts[32]; timestamp(ts, sizeof(ts));
        const char* name = (sig == SIGSEGV) ? "SIGSEGV"
                         : (sig == SIGABRT) ? "SIGABRT"
                         : (sig == SIGFPE)  ? "SIGFPE"
                         : (sig == SIGILL)  ? "SIGILL" : "signal";
        fprintf(g_log, "%s [FATAL] *** CRASH: %s (%d) — see above for the last log line ***\n",
                ts, name, sig);
        fflush(g_log);
        close_log();
    }
    std::signal(sig, SIG_DFL);
    std::raise(sig);   // let the OS produce its normal crash report / core dump
}

void terminate_handler() {
    if (g_log) {
        char ts[32]; timestamp(ts, sizeof(ts));
        fprintf(g_log, "%s [FATAL] *** TERMINATE: uncaught exception ***\n", ts);
        fflush(g_log);
        close_log();
    }
    std::abort();
}

// Best-effort path to the executable's directory (works before InitWindow).
std::string exe_dir(const char* argv0) {
    std::error_code ec;
    std::filesystem::path p = std::filesystem::weakly_canonical(std::filesystem::path(argv0), ec);
    if (!ec && p.has_parent_path()) return p.parent_path().string();
    const char* d = GetApplicationDirectory();   // raylib fallback
    return d ? std::string(d) : std::string(".");
}

void init_logging(const char* argv0) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(exe_dir(argv0)) / "logs";
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path file = dir / "AfterAction.log";
    g_log = fopen(file.string().c_str(), "w");

    SetTraceLogCallback(trace_log_to_file);
    std::atexit(close_log);
    std::signal(SIGSEGV, crash_handler);
    std::signal(SIGABRT, crash_handler);
    std::signal(SIGFPE,  crash_handler);
    std::signal(SIGILL,  crash_handler);
    std::set_terminate(terminate_handler);
}

} // namespace

int main(int argc, char** argv) {
    init_logging(argv[0]);

    afteraction::AppConfig cfg;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (!strcmp(a, "--demo"))                         { cfg.demo_mode = true; }
        else if (!strcmp(a, "--port") && i+1<argc)        { cfg.udp_port = (uint16_t)atoi(argv[++i]); }
        else if (!strcmp(a, "--addr") && i+1<argc)        { cfg.bind_addr = argv[++i]; }
        else if (!strcmp(a, "--width") && i+1<argc)       { cfg.window_width  = atoi(argv[++i]); }
        else if (!strcmp(a, "--height") && i+1<argc)      { cfg.window_height = atoi(argv[++i]); }
        else if (!strcmp(a, "--help")) {
            puts("AfterAction — tactical telemetry viewer & debrief");
            puts("Usage: AfterAction [options]");
            puts("  --demo            Run built-in flight demo (no UDP needed)");
            puts("  --port  N         UDP listen port (default 22522)");
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
        TraceLog(LOG_INFO, "AfterAction starting — demo mode (no UDP)");
    else
        TraceLog(LOG_INFO, "AfterAction starting — listening UDP on %s:%u",
                 cfg.bind_addr.c_str(), cfg.udp_port);

    // Heap-allocate: Application embeds large fixed buffers (the TelemetryStore
    // ring is ~0.5 MB, plus the inbound queue). Keeping it off the stack leaves
    // the full stack available for the deep render/UI call path.
    auto app = std::make_unique<afteraction::Application>(cfg);
    app->run();

    TraceLog(LOG_INFO, "AfterAction exited cleanly");
    return 0;
}
