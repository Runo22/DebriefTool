#include "TerrainSystem.hpp"
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"   // raymath.h
#endif
#include <raymath.h>
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#include <rlgl.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>

namespace afteraction {

namespace {

constexpr double kRootHalf = 2097152.0;   // 2^21 m: the quadtree covers ±2097 km
constexpr int    kGrid     = 64;          // quads per chunk edge
constexpr int    kMaxLevel = 16;
constexpr size_t kMaxChunks = 1200;       // cache size before eviction (~210 MB of VRAM)
constexpr int    kUploadsPerFrame = 12;

// ── Shaders ───────────────────────────────────────────────────────────────────
const char* kVS = R"(#version 330
in vec3 vertexPosition;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
uniform float vScale;
out vec3 fragWorld;
out vec3 fragNormal;
out vec4 fragColor;
out float fragHeight;
void main() {
    fragWorld  = (matModel * vec4(vertexPosition, 1.0)).xyz;
    fragHeight = vertexPosition.y;
    // The model matrix scales Y by vScale; normals scale by its inverse.
    fragNormal = vec3(vertexNormal.x, vertexNormal.y / max(vScale, 0.01), vertexNormal.z);
    fragColor  = vertexColor;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

const char* kFS = R"(#version 330
in vec3 fragWorld;
in vec3 fragNormal;
in vec4 fragColor;
in float fragHeight;
uniform vec3 camPos;
uniform vec4 fogColor;
uniform vec2 fogRange;   // start, end (m)
uniform vec2 grid;       // spacing (m), alpha
uniform vec2 contour;    // step (m), alpha
uniform int  look;       // 1 = dark tactical, otherwise natural colours
uniform vec3 sunDir;
out vec4 finalColor;

// Anti-aliased line at integer values of `coord`, ~width_px pixels wide.
float lineMask(float coord, float width_px) {
    float d = max(fwidth(coord), 1e-5);
    float f = abs(fract(coord - 0.5) - 0.5);
    return 1.0 - smoothstep(0.0, d * width_px, f);
}

void main() {
    vec3 n = normalize(fragNormal);
    float diff = max(dot(n, sunDir), 0.0);
    vec3 base = (look == 1) ? vec3(0.050, 0.075, 0.115) * (0.55 + 0.65 * diff)
                            : fragColor.rgb * (0.28 + 0.85 * diff);

    if (grid.y > 0.0) {
        vec2 p  = fragWorld.xz / grid.x;
        vec2 fw = fwidth(p);
        float px   = 1.0 / max(max(fw.x, fw.y), 1e-6);      // pixels per grid cell
        float fade = smoothstep(5.0, 14.0, px);
        float g = max(lineMask(p.x, 1.3), lineMask(p.y, 1.3)) * fade * grid.y;
        base = mix(base, vec3(0.30, 0.66, 0.82), g);
    }
    if (contour.y > 0.0 && fragHeight > 0.5) {
        float c    = fragHeight / contour.x;
        float px   = 1.0 / max(fwidth(c), 1e-6);
        float fade = smoothstep(3.0, 9.0, px);
        float minor = lineMask(c, 1.0) * 0.45;
        float major = lineMask(c / 5.0, 1.8) * 0.85;
        base = mix(base, vec3(0.16, 0.10, 0.05), max(minor, major) * fade * contour.y);
    }

    float d = length(fragWorld - camPos);
    float f = clamp((d - fogRange.x) / max(fogRange.y - fogRange.x, 1.0), 0.0, 1.0);
    f = f * f * (3.0 - 2.0 * f);
    finalColor = vec4(mix(base, fogColor.rgb, f), 1.0);
}
)";

// Free a mesh's CPU arrays (never touches GL — safe on worker threads).
void free_cpu_mesh(Mesh& m) {
    RL_FREE(m.vertices); m.vertices = nullptr;
    RL_FREE(m.normals);  m.normals  = nullptr;
    RL_FREE(m.colors);   m.colors   = nullptr;
    RL_FREE(m.indices);  m.indices  = nullptr;
}

// DrawMesh only checks `indices != NULL` to pick indexed drawing; the real
// indices live on the GPU. Point uploaded meshes at this marker so their CPU
// arrays can be freed, and detach it again before UnloadMesh.
unsigned short g_index_marker[3] = {0, 0, 0};

void release_gpu_mesh(Mesh& m) {
    if (m.indices == g_index_marker) m.indices = nullptr;
    UnloadMesh(m);
}

// ── Colours ───────────────────────────────────────────────────────────────────
struct Stop { float h; float r, g, b; };
constexpr Stop kPalette[] = {
    {    0.0f,  76,  94,  60 },
    {  400.0f,  98, 108,  70 },
    {  900.0f, 128, 122,  86 },
    { 1400.0f, 146, 132, 100 },
    { 2200.0f, 150, 140, 122 },
    { 3000.0f, 176, 172, 164 },
    { 3800.0f, 228, 230, 232 },
};

void terrain_colour(float h, float ny, unsigned char* out) {
    if (h <= 0.05f) {                                    // sea level / sea
        out[0] = 26; out[1] = 46; out[2] = 74; out[3] = 255;
        return;
    }
    float r = kPalette[0].r, g = kPalette[0].g, b = kPalette[0].b;
    for (size_t i = 1; i < std::size(kPalette); ++i) {
        if (h <= kPalette[i].h) {
            const Stop& a = kPalette[i - 1];
            const Stop& c = kPalette[i];
            const float t = (h - a.h) / (c.h - a.h);
            r = a.r + (c.r - a.r) * t; g = a.g + (c.g - a.g) * t; b = a.b + (c.b - a.b) * t;
            break;
        }
        r = kPalette[i].r; g = kPalette[i].g; b = kPalette[i].b;
    }
    // Steep slopes read as bare rock.
    const float rock = std::clamp((0.86f - ny) / 0.22f, 0.0f, 1.0f);
    r += (118.0f - r) * rock; g += (110.0f - g) * rock; b += (102.0f - b) * rock;
    out[0] = static_cast<unsigned char>(r);
    out[1] = static_cast<unsigned char>(g);
    out[2] = static_cast<unsigned char>(b);
    out[3] = 255;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
TerrainSystem::TerrainSystem() = default;

TerrainSystem::~TerrainSystem() {
    {
        std::lock_guard<std::mutex> g(mu_);
        quit_ = true;
    }
    cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
    for (auto& b : done_) { free_cpu_mesh(b.mesh); }   // CPU arrays only
    // GPU meshes must already have been released by shutdown().
}

bool TerrainSystem::load_data(const std::string& data_dir) {
    std::string err;
    const size_t n = db_.add_directory(data_dir, &err);
    if (n == 0) {
        status_ = "no terrain packs in " + data_dir + (err.empty() ? "" : " (" + err + ")");
        TraceLog(LOG_WARNING, "Terrain: %s", status_.c_str());
        return false;
    }
    db_.set_cache_limit(size_t(384) << 20);
    status_ = std::to_string(db_.layers().size()) + " layers from " + std::to_string(n) + " packs";
    for (const auto& L : db_.layers())
        TraceLog(LOG_INFO, "Terrain layer %-14s %5.1f m  %.3f..%.3f N  %.3f..%.3f E",
                 L.name.c_str(), L.resolution_m, L.south(), L.north, L.west, L.east());
    return true;
}

void TerrainSystem::init_gpu() {
    shader_ = LoadShaderFromMemory(kVS, kFS);
    if (shader_.id == 0 || shader_.id == rlGetShaderIdDefault()) {
        TraceLog(LOG_WARNING, "Terrain: shader failed to compile; terrain disabled");
        return;
    }
    loc_cam_     = GetShaderLocation(shader_, "camPos");
    loc_fog_col_ = GetShaderLocation(shader_, "fogColor");
    loc_fog_rng_ = GetShaderLocation(shader_, "fogRange");
    loc_vscale_  = GetShaderLocation(shader_, "vScale");
    loc_grid_    = GetShaderLocation(shader_, "grid");
    loc_contour_ = GetShaderLocation(shader_, "contour");
    loc_look_    = GetShaderLocation(shader_, "look");
    loc_sun_     = GetShaderLocation(shader_, "sunDir");
    material_ = LoadMaterialDefault();
    material_.shader = shader_;
    gpu_ready_ = true;

    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    const unsigned n  = std::min(3u, hw - 1);
    for (unsigned i = 0; i < n; ++i) workers_.emplace_back(&TerrainSystem::worker_main, this);
}

void TerrainSystem::shutdown() {
    {
        std::lock_guard<std::mutex> g(mu_);
        quit_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
    clear_chunks();
    for (auto& b : done_) { free_cpu_mesh(b.mesh); }
    done_.clear();
    if (gpu_ready_) {
        material_.shader = Shader{};   // the shader is unloaded separately
        UnloadShader(shader_);
        RL_FREE(material_.maps);
        material_.maps = nullptr;
        gpu_ready_ = false;
    }
}

void TerrainSystem::clear_chunks() {
    for (auto& [k, c] : chunks_)
        if (c.uploaded) release_gpu_mesh(c.mesh);
    chunks_.clear();
    draw_list_.clear();
    gpu_bytes_ = 0.0;
}

void TerrainSystem::set_frame(const GeoFrame& frame) {
    if (frame_.valid() && frame.valid() && frame_.lat0() == frame.lat0() && frame_.lon0() == frame.lon0())
        return;
    frame_ = frame;
    clear_chunks();
    std::lock_guard<std::mutex> g(mu_);
    worker_frame_ = frame;
    ++epoch_;
    queue_.clear();
    for (auto& b : done_) { free_cpu_mesh(b.mesh); }
    done_.clear();
}

// ── Height queries ────────────────────────────────────────────────────────────
float TerrainSystem::height_at(float x, float z) const {
    if (!frame_.valid() || db_.empty()) return 0.0f;
    double lat, lon;
    frame_.to_geo(x, z, &lat, &lon);
    return db_.height_or(lat, lon, 0.0f);
}

std::optional<float> TerrainSystem::height_at_geo(double lat, double lon) const {
    if (db_.empty()) return std::nullopt;
    return db_.height(lat, lon);
}

float TerrainSystem::resolution_at(float x, float z) const {
    if (!frame_.valid()) return 0.0f;
    double lat, lon;
    frame_.to_geo(x, z, &lat, &lon);
    return db_.resolution_at(lat, lon);
}

bool TerrainSystem::raycast(const Ray& ray, float vscale, float max_dist, Vector3* hit) const {
    auto above = [&](float t, float* gap) {
        const Vector3 p = Vector3Add(ray.position, Vector3Scale(ray.direction, t));
        *gap = p.y - height_at(p.x, p.z) * vscale;
        return *gap > 0.0f;
    };
    float gap = 0.0f;
    if (!above(0.0f, &gap)) return false;              // camera under the surface
    float t0 = 0.0f, t = 0.0f;
    while (t < max_dist) {
        // Step proportional to the clearance (and distance) — safe for slopes < 45°.
        const float step = std::max({2.0f, gap * 0.5f, t * 0.002f});
        t0 = t;
        t += step;
        if (!above(t, &gap)) {
            float lo = t0, hi = t;
            for (int i = 0; i < 24; ++i) {
                const float mid = 0.5f * (lo + hi);
                float g2;
                if (above(mid, &g2)) lo = mid; else hi = mid;
            }
            *hit = Vector3Add(ray.position, Vector3Scale(ray.direction, hi));
            return true;
        }
    }
    return false;
}

std::vector<unsigned char> TerrainSystem::render_map(const GeoFrame& frame, float cx, float cz,
                                                     float range, int size) const {
    std::vector<unsigned char> rgba(size_t(size) * size * 4, 0);
    if (!frame.valid()) return rgba;
    const double sp = double(range) / size;
    const int G = size + 2;
    std::vector<float> h(size_t(G) * G);
    double north, west;
    frame.to_geo(cx - range * 0.5 - sp, cz - range * 0.5 - sp, &north, &west);
    db_.sample_grid(north, west, sp / frame.m_per_deg_lat(), sp / frame.m_per_deg_lon(), G, G,
                    h.data(), 0.0f, static_cast<float>(sp * 0.5));
    const Vector3 sun = kSunDir;
    for (int j = 0; j < size; ++j) {
        for (int i = 0; i < size; ++i) {
            auto H = [&](int a, int b) { return h[size_t(b + 1) * G + size_t(a + 1)]; };
            const float hv = H(i, j);
            // Relief exaggerated ×3 so gentle terrain still reads on a small map.
            const float dx = (H(i + 1, j) - H(i - 1, j)) * 3.0f / float(2 * sp);
            const float dz = (H(i, j + 1) - H(i, j - 1)) * 3.0f / float(2 * sp);
            const Vector3 n = Vector3Normalize({ -dx, 1.0f, -dz });
            const float shade = 0.40f + 0.75f * std::max(0.0f, Vector3DotProduct(n, sun));
            unsigned char* px = &rgba[(size_t(j) * size + i) * 4];
            terrain_colour(hv, 1.0f, px);
            for (int c = 0; c < 3; ++c)
                px[c] = static_cast<unsigned char>(std::min(255.0f, px[c] * shade * 0.85f));
            px[3] = 255;
        }
    }
    return rgba;
}

// ── Quadtree ──────────────────────────────────────────────────────────────────
void TerrainSystem::chunk_bounds(int level, int ix, int iz, double* x0, double* z0, double* size) const {
    *size = 2.0 * kRootHalf / double(1 << level);
    *x0 = -kRootHalf + ix * *size;
    *z0 = -kRootHalf + iz * *size;
}

float TerrainSystem::finest_resolution(double x0, double z0, double size) const {
    double lat_n, lon_w, lat_s, lon_e;
    frame_.to_geo(x0, z0, &lat_n, &lon_w);
    frame_.to_geo(x0 + size, z0 + size, &lat_s, &lon_e);
    float best = 1e9f;
    for (const auto& L : db_.layers())
        if (L.north >= lat_s && L.south() <= lat_n && L.east() >= lon_w && L.west <= lon_e)
            best = std::min(best, L.resolution_m);
    return best;
}

void TerrainSystem::select(int level, int ix, int iz, const Camera3D& cam, const TerrainStyle& style,
                           std::vector<Request>& requests) {
    double x0, z0, size;
    chunk_bounds(level, ix, iz, &x0, &z0, &size);
    const Key key = make_key(level, ix, iz);
    auto it = chunks_.find(key);
    const bool ready = it != chunks_.end() && it->second.uploaded;

    // Distance from the camera to the chunk's box.
    const float hmin = ready ? it->second.hmin * style.vscale : 0.0f;
    const float hmax = ready ? it->second.hmax * style.vscale : 0.0f;
    const double dx = std::max({x0 - cam.position.x, 0.0, cam.position.x - (x0 + size)});
    const double dz = std::max({z0 - cam.position.z, 0.0, cam.position.z - (z0 + size)});
    const double dy = std::max({double(hmin) - cam.position.y, 0.0, cam.position.y - double(hmax)});
    const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);

    // Cull chunks outside the view frustum or beyond the fog (invisible anyway).
    // Unbuilt chunks get a generous height range so they are never culled wrongly.
    const float y0 = ready ? hmin - 50.0f : -500.0f * style.vscale;
    const float y1 = ready ? hmax + 50.0f :  9000.0f * style.vscale;
    if (dist > style.fog_end * 1.05 ||
        !box_visible(float(x0), y0, float(z0), float(x0 + size), y1, float(z0 + size))) {
        if (ready) it->second.last_used = frame_no_;
        return;
    }

    const double spacing = size / kGrid;
    const bool leaf = level >= kMaxLevel || spacing <= 0.8 * finest_resolution(x0, z0, size);
    const bool want_split = !leaf && dist < size * style.detail * split_scale_;

    if (want_split) {
        bool all = true;
        for (int c2 = 0; c2 < 4; ++c2) {
            const Key ck = make_key(level + 1, ix * 2 + (c2 & 1), iz * 2 + (c2 >> 1));
            auto cit = chunks_.find(ck);
            if (cit == chunks_.end() || !cit->second.uploaded) {
                all = false;
                requests.push_back({ck, float(dist / size)});
            }
        }
        if (all || !ready) {
            if (all) {
                for (int c2 = 0; c2 < 4; ++c2)
                    select(level + 1, ix * 2 + (c2 & 1), iz * 2 + (c2 >> 1), cam, style, requests);
                if (ready) it->second.last_used = frame_no_;   // keep the parent warm
                return;
            }
        }
    }
    if (ready) {
        it->second.last_used = frame_no_;
        draw_list_.push_back(key);
    } else {
        requests.push_back({key, float(dist / size) - 1.0f});   // missing coverage first
    }
}

bool TerrainSystem::box_visible(float x0, float y0, float z0, float x1, float y1, float z1) const {
    for (const auto& p : planes_) {
        // The box corner furthest along the plane normal.
        const float x = p[0] >= 0 ? x1 : x0;
        const float y = p[1] >= 0 ? y1 : y0;
        const float z = p[2] >= 0 ? z1 : z0;
        if (p[0] * x + p[1] * y + p[2] * z + p[3] < 0.0f) return false;
    }
    return true;
}

void TerrainSystem::update(const Camera3D& cam, const TerrainStyle& style) {
    ++frame_no_;
    draw_list_.clear();
    if (!gpu_ready_ || !frame_.valid() || db_.empty() || style.look == TerrainLook::None) return;

    // View frustum (Gribb–Hartmann planes from view × projection). The far plane
    // is left out: distance is limited by the fog test instead.
    {
        const float aspect = float(GetScreenWidth()) / float(std::max(1, GetScreenHeight()));
        const Matrix view = MatrixLookAt(cam.position, cam.target, cam.up);
        const Matrix proj = MatrixPerspective(cam.fovy * DEG2RAD, aspect, 1.0, 1.0e7);
        const Matrix m = MatrixMultiply(view, proj);
        const float rows[4][4] = { { m.m0, m.m4, m.m8,  m.m12 }, { m.m1, m.m5, m.m9,  m.m13 },
                                   { m.m2, m.m6, m.m10, m.m14 }, { m.m3, m.m7, m.m11, m.m15 } };
        for (int i = 0; i < 4; ++i) {                     // left, right, bottom, top
            const int r = i / 2;
            const float sgn = (i % 2 == 0) ? 1.0f : -1.0f;
            for (int k = 0; k < 4; ++k) planes_[i][k] = rows[3][k] + sgn * rows[r][k];
        }
        for (int k = 0; k < 4; ++k) planes_[4][k] = rows[3][k] + rows[2][k];   // near
        // Pixel density: at 900 px of screen height the defaults look right.
        split_scale_ = std::max(0.5f, float(GetScreenHeight()) / 900.0f);
    }

    // 1. Upload finished builds (bounded per frame).
    std::vector<Built> ready;
    {
        std::lock_guard<std::mutex> g(mu_);
        const size_t n = std::min<size_t>(done_.size(), kUploadsPerFrame);
        ready.assign(std::make_move_iterator(done_.begin()), std::make_move_iterator(done_.begin() + n));
        done_.erase(done_.begin(), done_.begin() + n);
        for (auto& b : ready) in_flight_.erase(b.key);
    }
    for (auto& b : ready) {
        if (b.epoch != epoch_ || chunks_.count(b.key)) {
            free_cpu_mesh(b.mesh);
            continue;
        }
        UploadMesh(&b.mesh, false);
        free_cpu_mesh(b.mesh);   // the GPU copy is all DrawMesh needs
        b.mesh.indices = g_index_marker;
        Chunk c;
        c.mesh = b.mesh;
        c.uploaded = true;
        c.hmin = b.hmin;
        c.hmax = b.hmax;
        c.last_used = frame_no_;
        gpu_bytes_ += double(b.mesh.vertexCount) * (12 + 12 + 4) + double(b.mesh.triangleCount) * 6;
        chunks_.emplace(b.key, c);
    }

    // 2. Choose chunks for this view; collect what's missing.
    std::vector<Request> requests;
    select(0, 0, 0, cam, style, requests);
    std::sort(requests.begin(), requests.end(),
              [](const Request& a, const Request& b) { return a.priority < b.priority; });
    {
        std::lock_guard<std::mutex> g(mu_);
        queue_.clear();
        for (const auto& r : requests)
            if (!in_flight_.count(r.key)) queue_.push_back(r);
        queued_ = int(queue_.size());
    }
    cv_.notify_all();

    // 3. Evict chunks not used recently once the cache is large.
    if (chunks_.size() > kMaxChunks) {
        std::vector<std::pair<uint64_t, Key>> old;
        for (auto& [k, c] : chunks_)
            if (c.last_used + 2 < frame_no_) old.push_back({c.last_used, k});
        std::sort(old.begin(), old.end());
        const size_t drop = std::min(old.size(), chunks_.size() - kMaxChunks * 3 / 4);
        for (size_t i = 0; i < drop; ++i) {
            auto it = chunks_.find(old[i].second);
            gpu_bytes_ -= double(it->second.mesh.vertexCount) * 28 + double(it->second.mesh.triangleCount) * 6;
            release_gpu_mesh(it->second.mesh);
            chunks_.erase(it);
        }
    }
}

void TerrainSystem::draw(const Camera3D& cam, const TerrainStyle& style) {
    if (draw_list_.empty()) return;

    const float cam_pos[3] = { cam.position.x, cam.position.y, cam.position.z };
    const float fog_col[4] = { style.fog.r / 255.0f, style.fog.g / 255.0f, style.fog.b / 255.0f, 1.0f };
    const float fog_rng[2] = { style.fog_start, style.fog_end };
    const bool  grid_on = style.look == TerrainLook::Grid || style.look == TerrainLook::ShadedGrid;
    const float grid[2] = { style.grid_spacing, grid_on ? (style.look == TerrainLook::Grid ? 0.9f : 0.45f) : 0.0f };
    const float contour[2] = { style.contour_step, style.look == TerrainLook::Contours ? 1.0f : 0.0f };
    const int   look = style.look == TerrainLook::Grid ? 1 : 2;
    const float sun_v[3] = { kSunDir.x, kSunDir.y, kSunDir.z };
    SetShaderValue(shader_, loc_cam_, cam_pos, SHADER_UNIFORM_VEC3);
    SetShaderValue(shader_, loc_fog_col_, fog_col, SHADER_UNIFORM_VEC4);
    SetShaderValue(shader_, loc_fog_rng_, fog_rng, SHADER_UNIFORM_VEC2);
    SetShaderValue(shader_, loc_vscale_, &style.vscale, SHADER_UNIFORM_FLOAT);
    SetShaderValue(shader_, loc_grid_, grid, SHADER_UNIFORM_VEC2);
    SetShaderValue(shader_, loc_contour_, contour, SHADER_UNIFORM_VEC2);
    SetShaderValue(shader_, loc_look_, &look, SHADER_UNIFORM_INT);
    SetShaderValue(shader_, loc_sun_, sun_v, SHADER_UNIFORM_VEC3);

    const Matrix xf = MatrixScale(1.0f, style.vscale, 1.0f);
    rlDrawRenderBatchActive();
    rlDisableBackfaceCulling();   // skirts face both ways
    for (Key k : draw_list_) {
        auto it = chunks_.find(k);
        if (it != chunks_.end()) DrawMesh(it->second.mesh, material_, xf);
    }
    rlEnableBackfaceCulling();
}

TerrainStats TerrainSystem::stats() const {
    TerrainStats s;
    s.drawn = int(draw_list_.size());
    s.cached = int(chunks_.size());
    s.queued = queued_;
    s.gpu_mb = gpu_bytes_ / 1e6;
    return s;
}

// ── Workers ───────────────────────────────────────────────────────────────────
void TerrainSystem::worker_main() {
    for (;;) {
        Key key;
        GeoFrame frame;
        uint64_t epoch;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [&] { return quit_ || !queue_.empty(); });
            if (quit_) return;
            const Request r = queue_.front();
            queue_.erase(queue_.begin());
            if (in_flight_.count(r.key)) continue;
            in_flight_.insert(r.key);
            key = r.key;
            frame = worker_frame_;
            epoch = epoch_;
        }
        Built b = build_chunk(key, frame, epoch);
        std::lock_guard<std::mutex> g(mu_);
        if (quit_ || b.epoch != epoch_) {
            in_flight_.erase(key);
            free_cpu_mesh(b.mesh);
            continue;
        }
        done_.push_back(std::move(b));
    }
}

TerrainSystem::Built TerrainSystem::build_chunk(Key key, const GeoFrame& frame, uint64_t epoch) const {
    int level, ix, iz;
    split_key(key, &level, &ix, &iz);
    double x0, z0, size;
    chunk_bounds(level, ix, iz, &x0, &z0, &size);
    const double sp = size / kGrid;

    // Heights on an (N+3)² lat/lon grid including a one-sample border for normals.
    // The ENU→geo mapping is linear, so the world grid is a regular lat/lon grid.
    constexpr int G = kGrid + 3;
    std::vector<float> h(size_t(G) * G);
    double north, west;
    frame.to_geo(x0 - sp, z0 - sp, &north, &west);
    db_.sample_grid(north, west, sp / frame.m_per_deg_lat(), sp / frame.m_per_deg_lon(), G, G,
                    h.data(), 0.0f, static_cast<float>(sp * 0.5));
    auto H = [&](int i, int j) { return h[size_t(j + 1) * G + size_t(i + 1)]; };

    constexpr int V = kGrid + 1;
    const int skirt_verts = 4 * V;
    Built b;
    b.key = key;
    b.epoch = epoch;
    Mesh& m = b.mesh;
    m.vertexCount   = V * V + skirt_verts;
    m.triangleCount = kGrid * kGrid * 2 + 4 * kGrid * 2;
    m.vertices = static_cast<float*>(RL_MALLOC(sizeof(float) * 3 * m.vertexCount));
    m.normals  = static_cast<float*>(RL_MALLOC(sizeof(float) * 3 * m.vertexCount));
    m.colors   = static_cast<unsigned char*>(RL_MALLOC(4 * m.vertexCount));
    m.indices  = static_cast<unsigned short*>(RL_MALLOC(sizeof(unsigned short) * 3 * m.triangleCount));

    float hmin = 1e9f, hmax = -1e9f;
    for (int j = 0; j < V; ++j) {
        for (int i = 0; i < V; ++i) {
            const int v = j * V + i;
            const float hv = H(i, j);
            m.vertices[v * 3 + 0] = float(x0 + i * sp);
            m.vertices[v * 3 + 1] = hv;
            m.vertices[v * 3 + 2] = float(z0 + j * sp);
            const float dhdx = (H(i + 1, j) - H(i - 1, j)) / float(2 * sp);
            const float dhdz = (H(i, j + 1) - H(i, j - 1)) / float(2 * sp);
            const Vector3 n = Vector3Normalize({ -dhdx, 1.0f, -dhdz });
            m.normals[v * 3 + 0] = n.x;
            m.normals[v * 3 + 1] = n.y;
            m.normals[v * 3 + 2] = n.z;
            terrain_colour(hv, n.y, &m.colors[v * 4]);
            hmin = std::min(hmin, hv);
            hmax = std::max(hmax, hv);
        }
    }
    b.hmin = hmin;
    b.hmax = hmax;

    int t = 0;
    auto tri = [&](int a, int c, int d) {
        m.indices[t++] = static_cast<unsigned short>(a);
        m.indices[t++] = static_cast<unsigned short>(c);
        m.indices[t++] = static_cast<unsigned short>(d);
    };
    for (int j = 0; j < kGrid; ++j)
        for (int i = 0; i < kGrid; ++i) {
            const int a = j * V + i, c = a + V;
            tri(a, c, a + 1);
            tri(a + 1, c, c + 1);
        }

    // Skirts: a curtain hanging below each edge hides cracks against coarser neighbours.
    const float depth = std::max(15.0f, float(size) * 0.015f);
    int sv = V * V;
    auto edge_vertex = [&](int e, int k) {
        switch (e) {
            case 0:  return k;                     // north edge (j = 0)
            case 1:  return kGrid * V + k;         // south edge (j = N)
            case 2:  return k * V;                 // west edge  (i = 0)
            default: return k * V + kGrid;         // east edge  (i = N)
        }
    };
    for (int e = 0; e < 4; ++e) {
        const int base = sv;
        for (int k = 0; k < V; ++k) {
            const int src = edge_vertex(e, k);
            std::memcpy(&m.vertices[sv * 3], &m.vertices[src * 3], sizeof(float) * 3);
            m.vertices[sv * 3 + 1] -= depth;
            std::memcpy(&m.normals[sv * 3], &m.normals[src * 3], sizeof(float) * 3);
            std::memcpy(&m.colors[sv * 4], &m.colors[src * 4], 4);
            ++sv;
        }
        for (int k = 0; k < kGrid; ++k) {
            const int a = edge_vertex(e, k), c = edge_vertex(e, k + 1);
            tri(a, base + k, c);
            tri(c, base + k, base + k + 1);
        }
    }
    return b;
}

} // namespace afteraction
