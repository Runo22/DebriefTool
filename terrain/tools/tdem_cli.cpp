// tdem_cli — inspect and query TDEM packs.
//
//   tdem info  <dir|file>...
//   tdem query <dir|file> <lat> <lon> [<lat> <lon> ...]
//   tdem bench <dir|file> [samples]
#include "tdem/tdem.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>

static bool open_any(tdem::Database& db, const std::string& p) {
    std::string err;
    const bool ok = std::filesystem::is_directory(p) ? db.add_directory(p, &err) > 0
                                                     : db.add_pack(p, &err);
    if (!ok) std::fprintf(stderr, "error: %s\n", err.empty() ? p.c_str() : err.c_str());
    return ok;
}

static int usage() {
    std::fprintf(stderr,
                 "usage:\n"
                 "  tdem info  <dir|file>...\n"
                 "  tdem query <dir|file> <lat> <lon> [<lat> <lon> ...]\n"
                 "  tdem bench <dir|file> [samples]\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1];
    tdem::Database db;

    if (cmd == "info") {
        for (int i = 2; i < argc; ++i)
            if (!open_any(db, argv[i])) return 1;
        std::printf("%-14s %8s %8s %9s %9s %9s %9s %7s %6s %8s %8s\n", "layer", "rows", "cols",
                    "south", "north", "west", "east", "res_m", "q_m", "min_h", "max_h");
        for (const auto& L : db.layers())
            std::printf("%-14s %8u %8u %9.4f %9.4f %9.4f %9.4f %7.1f %6.2f %8.1f %8.1f\n",
                        L.name.c_str(), L.rows, L.cols, L.south(), L.north, L.west, L.east(),
                        L.resolution_m, L.quant_step, L.min_h, L.max_h);
        return 0;
    }
    if (cmd == "query") {
        if (argc < 5 || (argc - 3) % 2) return usage();
        if (!open_any(db, argv[2])) return 1;
        for (int i = 3; i + 1 < argc; i += 2) {
            const double lat = std::atof(argv[i]), lon = std::atof(argv[i + 1]);
            auto h = db.height(lat, lon);
            if (h) std::printf("%.6f %.6f  %8.2f m  (%.1f m grid)\n", lat, lon, *h, db.resolution_at(lat, lon));
            else   std::printf("%.6f %.6f  not covered\n", lat, lon);
        }
        return 0;
    }
    if (cmd == "bench") {
        if (!open_any(db, argv[2])) return 1;
        const int n = argc > 3 ? std::atoi(argv[3]) : 1000000;
        std::mt19937 rng(42);
        std::uniform_real_distribution<double> la(39.9, 40.3), lo(32.3, 32.8);   // Akinci / Ankara
        auto t0 = std::chrono::steady_clock::now();
        double acc = 0;
        for (int i = 0; i < n; ++i) acc += db.height_or(la(rng), lo(rng));
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("%d random samples: %.3f s (%.0f ns/sample), mean %.1f m, cache %.1f MB\n", n, s,
                    1e9 * s / n, acc / n, db.cache_bytes() / 1e6);
        return 0;
    }
    return usage();
}
