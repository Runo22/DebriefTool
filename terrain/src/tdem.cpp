// tdem.cpp — TDEM pack reader. Format: see FORMAT.md.
#include "tdem/tdem.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <list>
#include <mutex>
#include <unordered_map>

// The vendored inflater exports two extern "C" functions that raylib (and other
// libraries) also embed; rename ours so they can live in the same binary.
#define sinflate  tdem_sinflate_impl
#define zsinflate tdem_zsinflate_impl
#define SINFL_IMPLEMENTATION
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#  pragma GCC diagnostic ignored "-Wunused-function"
#elif defined(_MSC_VER)
#  pragma warning(push, 0)
#endif
#include "../third_party/sinfl.h"
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#  pragma warning(pop)
#endif
#undef sinflate
#undef zsinflate

namespace tdem {
namespace {

constexpr char     kMagic[4]        = {'T', 'D', 'E', 'M'};
constexpr uint32_t kVersion         = 1;
constexpr size_t   kFileHeaderSize  = 64;
constexpr size_t   kLayerHeaderSize = 128;
constexpr size_t   kTileEntrySize   = 24;

// Little-endian field readers (the format is LE regardless of host).
uint32_t rd_u32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
uint64_t rd_u64(const uint8_t* p) { return uint64_t(rd_u32(p)) | uint64_t(rd_u32(p + 4)) << 32; }
int32_t  rd_i32(const uint8_t* p) { return static_cast<int32_t>(rd_u32(p)); }
float rd_f32(const uint8_t* p) {
    uint32_t u = rd_u32(p);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
double rd_f64(const uint8_t* p) {
    uint64_t u = rd_u64(p);
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}

constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

float smoothstep01(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

struct TileEntry {
    uint64_t offset = 0;   // 0 => absent (all heights 0 m, i.e. sea)
    uint32_t size   = 0;
    int32_t  base   = 0;   // tile minimum, in quant steps
    float    min_h = 0.0f, max_h = 0.0f;
};

struct PackFile {
    std::string   path;
    std::ifstream in;
    std::mutex    io;
};

struct Layer {
    LayerInfo              info;
    PackFile*              pack = nullptr;
    uint32_t               tiles_x = 0, tiles_y = 0;
    std::vector<TileEntry> tiles;
    double                 m_per_row = 0.0, m_per_col = 0.0;   // for edge blending
};

using Tile = std::vector<float>;   // tile_size * tile_size heights, row-major

} // namespace

struct Database::Impl {
    std::vector<std::unique_ptr<PackFile>> packs;
    std::vector<Layer>                     layers;     // finest first
    std::vector<LayerInfo>                 infos;      // mirrors layers

    // LRU cache of decoded tiles.
    mutable std::mutex cache_mu;
    using Key = uint64_t;
    mutable std::list<Key> lru;                        // front = most recent
    struct Slot { std::shared_ptr<const Tile> tile; std::list<Key>::iterator it; };
    mutable std::unordered_map<Key, Slot> cache;
    mutable size_t cache_used = 0;
    size_t         cache_limit = size_t(256) << 20;
    mutable std::unordered_map<uint32_t, std::shared_ptr<const Tile>> zero_tiles;

    std::shared_ptr<const Tile> decode(const Layer& L, const TileEntry& e) const;
    std::shared_ptr<const Tile> tile(size_t li, uint32_t tx, uint32_t ty) const;
    float sample(size_t li, uint32_t r, uint32_t c) const;
    std::optional<float> bilinear(size_t li, double lat, double lon, float* edge_w) const;
    std::optional<float> blended(double lat, double lon, float min_res) const;
    void evict_locked() const;
};

// ── Tile decoding ─────────────────────────────────────────────────────────────

std::shared_ptr<const Tile> Database::Impl::decode(const Layer& L, const TileEntry& e) const {
    const uint32_t T = L.info.tile_size;
    const size_t   n = size_t(T) * T;
    std::vector<uint8_t> blob(e.size);
    {
        std::lock_guard<std::mutex> g(L.pack->io);
        L.pack->in.clear();
        L.pack->in.seekg(static_cast<std::streamoff>(e.offset));
        L.pack->in.read(reinterpret_cast<char*>(blob.data()), e.size);
        if (!L.pack->in) return nullptr;
    }
    // sinfl may write up to a few bytes past the logical end; give it slack.
    std::vector<uint8_t> planes(2 * n + 64);
    const int got = tdem_sinflate_impl(planes.data(), static_cast<int>(planes.size()),
                                       blob.data(), static_cast<int>(blob.size()));
    if (got != static_cast<int>(2 * n)) return nullptr;

    // Undo zigzag, then the planar predictor: v is the 2D prefix sum of the
    // residuals, all in wrapping uint16 arithmetic.
    std::vector<uint16_t> v(n);
    for (uint32_t i = 0; i < T; ++i) {
        uint16_t row_acc = 0;
        for (uint32_t j = 0; j < T; ++j) {
            const size_t   k  = size_t(i) * T + j;
            const uint16_t zz = uint16_t(planes[k] | (planes[n + k] << 8));
            const uint16_t r  = uint16_t((zz >> 1) ^ uint16_t(0u - (zz & 1u)));
            row_acc = uint16_t(row_acc + r);
            v[k] = uint16_t(row_acc + (i ? v[k - T] : 0));
        }
    }
    auto out = std::make_shared<Tile>(n);
    const float step = L.info.quant_step;
    for (size_t k = 0; k < n; ++k)
        (*out)[k] = static_cast<float>((int64_t(e.base) + v[k]) * double(step));
    return out;
}

void Database::Impl::evict_locked() const {
    while (cache_used > cache_limit && !lru.empty()) {
        auto it = cache.find(lru.back());
        cache_used -= it->second.tile->size() * sizeof(float);
        cache.erase(it);
        lru.pop_back();
    }
}

std::shared_ptr<const Tile> Database::Impl::tile(size_t li, uint32_t tx, uint32_t ty) const {
    const Layer& L = layers[li];
    const TileEntry& e = L.tiles[size_t(ty) * L.tiles_x + tx];
    const Key key = (uint64_t(li) << 48) | (uint64_t(ty) << 24) | tx;
    {
        std::lock_guard<std::mutex> g(cache_mu);
        if (e.offset == 0) {
            auto& z = zero_tiles[L.info.tile_size];
            if (!z) z = std::make_shared<Tile>(size_t(L.info.tile_size) * L.info.tile_size, 0.0f);
            return z;
        }
        auto it = cache.find(key);
        if (it != cache.end()) {
            lru.splice(lru.begin(), lru, it->second.it);
            return it->second.tile;
        }
    }
    // Decode outside the cache lock so other threads keep sampling.
    auto t = decode(L, e);
    if (!t) return nullptr;
    std::lock_guard<std::mutex> g(cache_mu);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.tile;     // another thread won
    lru.push_front(key);
    cache.emplace(key, Slot{t, lru.begin()});
    cache_used += t->size() * sizeof(float);
    evict_locked();
    return t;
}

float Database::Impl::sample(size_t li, uint32_t r, uint32_t c) const {
    const uint32_t T = layers[li].info.tile_size;
    auto t = tile(li, c / T, r / T);
    return t ? (*t)[size_t(r % T) * T + (c % T)] : 0.0f;
}

std::optional<float> Database::Impl::bilinear(size_t li, double lat, double lon, float* edge_w) const {
    const Layer& L = layers[li];
    const LayerInfo& I = L.info;
    if (!I.contains(lat, lon)) return std::nullopt;
    const double fr = (I.north - lat) / I.dlat;
    const double fc = (lon - I.west) / I.dlon;
    const uint32_t r0 = std::min<uint32_t>(static_cast<uint32_t>(fr), I.rows > 1 ? I.rows - 2 : 0);
    const uint32_t c0 = std::min<uint32_t>(static_cast<uint32_t>(fc), I.cols > 1 ? I.cols - 2 : 0);
    const uint32_t r1 = std::min(r0 + 1, I.rows - 1), c1 = std::min(c0 + 1, I.cols - 1);
    const float ty = static_cast<float>(fr - r0), tx = static_cast<float>(fc - c0);

    float h00, h01, h10, h11;
    const uint32_t T = I.tile_size;
    if (r0 / T == r1 / T && c0 / T == c1 / T) {          // common case: one tile
        auto t = tile(li, c0 / T, r0 / T);
        if (!t) return std::nullopt;
        const size_t base = size_t(r0 % T) * T + (c0 % T);
        h00 = (*t)[base];
        h01 = (*t)[base + (c1 - c0)];
        h10 = (*t)[base + (r1 - r0) * size_t(T)];
        h11 = (*t)[base + (r1 - r0) * size_t(T) + (c1 - c0)];
    } else {
        h00 = sample(li, r0, c0); h01 = sample(li, r0, c1);
        h10 = sample(li, r1, c0); h11 = sample(li, r1, c1);
    }
    if (edge_w) {
        if (I.blend_m > 0.0f) {
            const double dr = std::min(fr, (I.rows - 1) - fr) * L.m_per_row;
            const double dc = std::min(fc, (I.cols - 1) - fc) * L.m_per_col;
            *edge_w = smoothstep01(static_cast<float>(std::min(dr, dc) / I.blend_m));
        } else {
            *edge_w = 1.0f;
        }
    }
    const float top = h00 + (h01 - h00) * tx;
    const float bot = h10 + (h11 - h10) * tx;
    return top + (bot - top) * ty;
}

std::optional<float> Database::Impl::blended(double lat, double lon, float min_res) const {
    // Finest first: h = w1*h1 + (1-w1)*(w2*h2 + (1-w2)*(...)).
    double sum = 0.0, remaining = 1.0;
    bool any = false;
    for (size_t li = 0; li < layers.size() && remaining > 1e-6; ++li) {
        if (layers[li].info.resolution_m < min_res * 0.999f) continue;
        float w = 1.0f;
        auto h = bilinear(li, lat, lon, &w);
        if (!h) continue;
        any = true;
        sum += remaining * w * *h;
        remaining *= (1.0 - w);
    }
    if (!any) return std::nullopt;
    // Outer edge of the coarsest covering layer: renormalise what we have.
    if (remaining > 1e-6) {
        if (remaining >= 1.0 - 1e-9) {
            // Only zero-weight hits (exactly on a border): take the finest one.
            for (size_t li = 0; li < layers.size(); ++li) {
                if (layers[li].info.resolution_m < min_res * 0.999f) continue;
                if (auto h = bilinear(li, lat, lon, nullptr)) return h;
            }
        }
        sum /= (1.0 - remaining);
    }
    return static_cast<float>(sum);
}

// ── Public API ────────────────────────────────────────────────────────────────

Database::Database() : impl_(std::make_unique<Impl>()) {}
Database::~Database() = default;

bool Database::add_pack(const std::string& path, std::string* err) {
    auto fail = [&](const std::string& why) {
        if (err) *err = path + ": " + why;
        return false;
    };
    auto pf = std::make_unique<PackFile>();
    pf->path = path;
    pf->in.open(path, std::ios::binary);
    if (!pf->in) return fail("cannot open");

    uint8_t hdr[kFileHeaderSize];
    if (!pf->in.read(reinterpret_cast<char*>(hdr), sizeof(hdr))) return fail("truncated header");
    if (std::memcmp(hdr, kMagic, 4) != 0) return fail("not a TDEM file");
    if (rd_u32(hdr + 4) != kVersion) return fail("unsupported version");
    const uint32_t n_layers = rd_u32(hdr + 8);
    const uint64_t ltab     = rd_u64(hdr + 16);
    if (n_layers == 0 || n_layers > 4096) return fail("bad layer count");

    std::vector<Layer> added;
    for (uint32_t i = 0; i < n_layers; ++i) {
        uint8_t b[kLayerHeaderSize];
        pf->in.seekg(static_cast<std::streamoff>(ltab + i * kLayerHeaderSize));
        if (!pf->in.read(reinterpret_cast<char*>(b), sizeof(b))) return fail("truncated layer table");
        Layer L;
        LayerInfo& I = L.info;
        I.name.assign(reinterpret_cast<const char*>(b), strnlen(reinterpret_cast<const char*>(b), 32));
        I.pack  = path;
        I.north = rd_f64(b + 32);
        I.west  = rd_f64(b + 40);
        I.dlat  = rd_f64(b + 48);
        I.dlon  = rd_f64(b + 56);
        const uint64_t ttab = rd_u64(b + 64);
        I.rows = rd_u32(b + 72);
        I.cols = rd_u32(b + 76);
        I.tile_size = rd_u32(b + 80);
        // b + 84: flags (reserved)
        I.quant_step   = rd_f32(b + 88);
        I.blend_m      = rd_f32(b + 92);
        I.resolution_m = rd_f32(b + 96);
        I.min_h        = rd_f32(b + 100);
        I.max_h        = rd_f32(b + 104);
        if (I.rows == 0 || I.cols == 0 || I.tile_size == 0 || I.tile_size > 4096 ||
            !(I.dlat > 0) || !(I.dlon > 0) || !(I.quant_step > 0))
            return fail("bad layer header '" + I.name + "'");

        L.tiles_x = (I.cols + I.tile_size - 1) / I.tile_size;
        L.tiles_y = (I.rows + I.tile_size - 1) / I.tile_size;
        const size_t nt = size_t(L.tiles_x) * L.tiles_y;
        std::vector<uint8_t> tb(nt * kTileEntrySize);
        pf->in.seekg(static_cast<std::streamoff>(ttab));
        if (!pf->in.read(reinterpret_cast<char*>(tb.data()), static_cast<std::streamsize>(tb.size())))
            return fail("truncated tile table");
        L.tiles.resize(nt);
        for (size_t k = 0; k < nt; ++k) {
            const uint8_t* p = tb.data() + k * kTileEntrySize;
            L.tiles[k] = TileEntry{rd_u64(p), rd_u32(p + 8), rd_i32(p + 12), rd_f32(p + 16), rd_f32(p + 20)};
        }
        const double mid = 0.5 * (I.north + I.south()) * kDegToRad;
        L.m_per_row = I.dlat * 111132.92;
        L.m_per_col = I.dlon * 111412.84 * std::cos(mid);
        L.pack = pf.get();
        added.push_back(std::move(L));
    }

    std::lock_guard<std::mutex> g(impl_->cache_mu);   // layer indices change: drop cache
    impl_->cache.clear();
    impl_->lru.clear();
    impl_->cache_used = 0;
    for (auto& L : added) impl_->layers.push_back(std::move(L));
    std::stable_sort(impl_->layers.begin(), impl_->layers.end(), [](const Layer& a, const Layer& b) {
        return a.info.resolution_m < b.info.resolution_m;
    });
    impl_->infos.clear();
    for (auto& L : impl_->layers) impl_->infos.push_back(L.info);
    impl_->packs.push_back(std::move(pf));
    return true;
}

size_t Database::add_directory(const std::string& dir, std::string* err) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file() && e.path().extension() == ".tdem") files.push_back(e.path());
    if (ec) {
        if (err) *err = dir + ": " + ec.message();
        return 0;
    }
    std::sort(files.begin(), files.end());
    size_t loaded = 0;
    for (const auto& f : files) {
        std::string e;
        if (add_pack(f.string(), &e)) ++loaded;
        else if (err) *err += (err->empty() ? "" : "; ") + e;
    }
    return loaded;
}

bool Database::empty() const { return impl_->layers.empty(); }
const std::vector<LayerInfo>& Database::layers() const { return impl_->infos; }

std::optional<float> Database::height(double lat, double lon) const {
    return impl_->blended(lat, lon, 0.0f);
}

std::optional<float> Database::height(double lat, double lon, float min_resolution_m) const {
    return impl_->blended(lat, lon, min_resolution_m);
}

float Database::height_or(double lat, double lon, float fallback) const {
    return impl_->blended(lat, lon, 0.0f).value_or(fallback);
}

std::optional<float> Database::layer_height(size_t layer_index, double lat, double lon) const {
    if (layer_index >= impl_->layers.size()) return std::nullopt;
    return impl_->bilinear(layer_index, lat, lon, nullptr);
}

size_t Database::sample_grid(double north, double west, double dlat, double dlon,
                             int rows, int cols, float* out,
                             float fallback, float min_resolution_m) const {
    size_t covered = 0;
    for (int r = 0; r < rows; ++r) {
        const double lat = north - r * dlat;
        for (int c = 0; c < cols; ++c) {
            auto h = impl_->blended(lat, west + c * dlon, min_resolution_m);
            out[size_t(r) * cols + c] = h.value_or(fallback);
            covered += h.has_value();
        }
    }
    return covered;
}

float Database::resolution_at(double lat, double lon) const {
    for (const auto& L : impl_->layers)
        if (L.info.contains(lat, lon)) return L.info.resolution_m;
    return 0.0f;
}

void Database::set_cache_limit(size_t bytes) {
    std::lock_guard<std::mutex> g(impl_->cache_mu);
    impl_->cache_limit = bytes;
    impl_->evict_locked();
}

size_t Database::cache_bytes() const {
    std::lock_guard<std::mutex> g(impl_->cache_mu);
    return impl_->cache_used;
}

void Database::clear_cache() {
    std::lock_guard<std::mutex> g(impl_->cache_mu);
    impl_->cache.clear();
    impl_->lru.clear();
    impl_->cache_used = 0;
}

} // namespace tdem
