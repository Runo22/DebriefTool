// tdem_test — checks the C++ reader against the Python reference.
//
// tests/vectors.json (written by tools/build_pack.py) lists exact grid-node
// samples per layer with the quantised height the encoder stored; every one
// must decode to the same value. Also checks blending, coverage and the C API.
#include "tdem/tdem.hpp"
#include "tdem/tdem_c.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

static int g_fail = 0;
#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            ++g_fail;                                                 \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);          \
            std::printf(__VA_ARGS__);                                 \
            std::printf("\n");                                        \
        }                                                             \
    } while (0)

struct Vec { std::string layer; double lat, lon, h; };

// Tiny parser for the flat {"samples": [{"layer":..,"lat":..,"lon":..,"h":..}]} file.
static std::vector<Vec> load_vectors(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string s = ss.str();
    std::vector<Vec> out;
    size_t p = 0;
    auto num = [&](const char* key, size_t from) {
        size_t k = s.find(std::string("\"") + key + "\":", from);
        return std::strtod(s.c_str() + k + std::strlen(key) + 3, nullptr);
    };
    while ((p = s.find("\"layer\":", p)) != std::string::npos) {
        size_t q0 = s.find('"', p + 8) + 1, q1 = s.find('"', q0);
        Vec v{s.substr(q0, q1 - q0), num("lat", p), num("lon", p), num("h", p)};
        out.push_back(v);
        p = q1;
    }
    return out;
}

int main(int argc, char** argv) {
    const std::string data = argc > 1 ? argv[1] : TDEM_TEST_DATA_DIR;
    const std::string vecs = argc > 2 ? argv[2] : TDEM_TEST_VECTORS;

    tdem::Database db;
    std::string err;
    const size_t packs = db.add_directory(data, &err);
    CHECK(packs > 0, "no packs loaded from %s: %s", data.c_str(), err.c_str());
    if (!packs) return 1;

    // Layers are sorted finest first.
    const auto& L = db.layers();
    for (size_t i = 1; i < L.size(); ++i)
        CHECK(L[i - 1].resolution_m <= L[i].resolution_m, "layer order");
    std::map<std::string, size_t> index;
    for (size_t i = 0; i < L.size(); ++i) index[L[i].name] = i;

    // 1. Exact grid nodes must decode to the stored quantised value.
    const auto v = load_vectors(vecs);
    CHECK(!v.empty(), "no vectors in %s", vecs.c_str());
    size_t checked = 0;
    for (const auto& s : v) {
        auto it = index.find(s.layer);
        if (it == index.end()) continue;
        auto h = db.layer_height(it->second, s.lat, s.lon);
        const double tol = 1e-3 + 1e-6 * std::fabs(s.h);
        CHECK(h && std::fabs(*h - s.h) <= tol, "%s (%.6f, %.6f): got %.3f want %.3f", s.layer.c_str(),
              s.lat, s.lon, h ? *h : NAN, s.h);
        ++checked;
    }
    std::printf("grid-node vectors: %zu checked\n", checked);

    // 2. Known places (heights of the runway profile, published ~832/843 m).
    struct Place { const char* name; double lat, lon, lo, hi; };
    const Place places[] = {
        {"Akinci RWY03 thr", 40.0662, 32.5550, 820, 845},
        {"Akinci RWY21 thr", 40.0916, 32.5763, 830, 855},
        {"Incirlik RWY05",   36.9937, 35.4122,  40,  65},
        {"Cigli RWY17",      38.5265, 27.0087,  -5,  15},
        {"Ankara Kizilay",   39.9208, 32.8541, 820, 920},
        {"Mt Erciyes",       38.5310, 35.4470, 3600, 3950},
        {"Black Sea",        42.3000, 34.0000,  -1,   1},
    };
    for (const auto& p : places) {
        auto h = db.height(p.lat, p.lon);
        CHECK(h && *h >= p.lo && *h <= p.hi, "%s: %.1f not in [%.0f, %.0f]", p.name, h ? *h : NAN, p.lo, p.hi);
        if (h) std::printf("  %-18s %8.1f m  (%.1f m grid)\n", p.name, *h, db.resolution_at(p.lat, p.lon));
    }
    CHECK(!db.height(10.0, 10.0), "point far outside Turkey must be uncovered");

    // 3. Blending is continuous across the Akinci 5 m layer edge.
    if (index.count("akinci_5m")) {
        const auto& A = L[index["akinci_5m"]];
        const double lon = 0.5 * (A.west + A.east());
        float prev = db.height_or(A.north + 0.002, lon);
        float max_jump = 0;
        for (int i = 1; i <= 400; ++i) {     // walk south across the north edge in ~1 m steps
            float h = db.height_or(A.north + 0.002 - i * 1e-5, lon);
            max_jump = std::fmax(max_jump, std::fabs(h - prev));
            prev = h;
        }
        CHECK(max_jump < 1.5f, "step across akinci_5m edge: %.2f m", max_jump);
    }

    // 4. LOD query skips fine layers.
    if (index.count("akinci_5m")) {
        auto fine = db.height(40.0789, 32.5656);
        auto coarse = db.height(40.0789, 32.5656, 100.0f);
        CHECK(fine && coarse && std::fabs(*fine - *coarse) < 30.0f, "lod query");
    }

    // 5. C API.
    tdem_db* c = tdem_open_dir(data.c_str());
    CHECK(c != nullptr, "tdem_open_dir");
    float hc = 0;
    CHECK(c && tdem_height(c, 40.0789, 32.5656, &hc) == 1 && std::fabs(hc - db.height_or(40.0789, 32.5656)) < 1e-4,
          "C API height");
    CHECK(tdem_layer_count(c) == static_cast<int>(L.size()), "C API layer count");
    tdem_close(c);

    std::printf(g_fail ? "%d FAILURE(S)\n" : "all tests passed\n", g_fail);
    return g_fail ? 1 : 0;
}
