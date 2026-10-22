#include "AssetManager.hpp"
#include "ProceduralModels.hpp"
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
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
#include <cstring>
#include <vector>

namespace afteraction {

namespace {

// GLSL 330. Vertex colour × material colour × texture; two-sided (the normal is
// turned toward the viewer); hemisphere ambient + sun + soft specular + rim;
// distance fog matching the terrain shader.
const char* kLitVS = R"(#version 330
in vec3 vertexPosition;
in vec2 vertexTexCoord;
in vec3 vertexNormal;
in vec4 vertexColor;
uniform mat4 mvp;
uniform mat4 matModel;
uniform mat4 matNormal;
out vec3 fragWorld;
out vec3 fragNormal;
out vec2 fragUV;
out vec4 fragColor;
void main() {
    fragWorld  = (matModel * vec4(vertexPosition, 1.0)).xyz;
    fragNormal = normalize((matNormal * vec4(vertexNormal, 0.0)).xyz);
    fragUV     = vertexTexCoord;
    fragColor  = vertexColor;
    gl_Position = mvp * vec4(vertexPosition, 1.0);
}
)";

const char* kLitFS = R"(#version 330
in vec3 fragWorld;
in vec3 fragNormal;
in vec2 fragUV;
in vec4 fragColor;
uniform sampler2D texture0;
uniform vec4 colDiffuse;
uniform vec3 camPos;
uniform vec3 sunDir;
uniform vec4 fogColor;
uniform vec2 fogRange;
out vec4 finalColor;
void main() {
    vec4 base = texture(texture0, fragUV) * colDiffuse * fragColor;
    vec3 v = normalize(camPos - fragWorld);
    vec3 n = normalize(fragNormal);
    if (dot(n, v) < 0.0) n = -n;
    float diff = max(dot(n, sunDir), 0.0);
    float sky  = 0.5 + 0.5 * n.y;
    vec3 amb   = mix(vec3(0.30, 0.28, 0.25), vec3(0.52, 0.58, 0.66), sky);
    vec3 h     = normalize(sunDir + v);
    float spec = pow(max(dot(n, h), 0.0), 32.0) * 0.28;
    float rim  = pow(1.0 - max(dot(n, v), 0.0), 3.0) * 0.18;
    vec3 c = base.rgb * (amb + vec3(1.0, 0.97, 0.90) * diff * 0.85) + vec3(spec + rim);
    float d = length(fragWorld - camPos);
    float f = clamp((d - fogRange.x) / max(fogRange.y - fogRange.x, 1.0), 0.0, 1.0);
    finalColor = vec4(mix(c, fogColor.rgb, f), base.a);
}
)";

// Largest bounding-box extent of a model, in model units.
float model_extent(const Model& m) {
    const BoundingBox bb = GetModelBoundingBox(m);
    return std::max({ bb.max.x - bb.min.x, bb.max.y - bb.min.y, bb.max.z - bb.min.z, 0.01f });
}

} // namespace

void AssetManager::init_procedural() {
    if (ready_) return;
    lit_ = LoadShaderFromMemory(kLitVS, kLitFS);
    loc_cam_     = GetShaderLocation(lit_, "camPos");
    loc_sun_     = GetShaderLocation(lit_, "sunDir");
    loc_fog_col_ = GetShaderLocation(lit_, "fogColor");
    loc_fog_rng_ = GetShaderLocation(lit_, "fogRange");
    // Defaults so the shader works without per-frame setup (e.g. in the hangar tool).
    set_frame_uniforms({ 0, 1000, 0 }, Vector3Normalize({ -0.55f, 0.75f, -0.45f }), { 0, 0, 0, 255 }, 1e9f, 2e9f);

    build_procedural_models(procedural_, fallback_);
    for (auto& [type, e] : procedural_) {
        apply_lit(e.model);
        for (Spinner& sp : e.spinners) apply_lit(sp.model);
    }
    apply_lit(fallback_.model);
    ready_ = true;
}

void AssetManager::set_frame_uniforms(Vector3 cam_pos, Vector3 sun_dir, Color fog,
                                      float fog_start, float fog_end) noexcept {
    if (lit_.id == 0) return;
    const float cam[3] = { cam_pos.x, cam_pos.y, cam_pos.z };
    const float sun[3] = { sun_dir.x, sun_dir.y, sun_dir.z };
    const float col[4] = { fog.r / 255.0f, fog.g / 255.0f, fog.b / 255.0f, 1.0f };
    const float rng[2] = { fog_start, fog_end };
    SetShaderValue(lit_, loc_cam_, cam, SHADER_UNIFORM_VEC3);
    SetShaderValue(lit_, loc_sun_, sun, SHADER_UNIFORM_VEC3);
    SetShaderValue(lit_, loc_fog_col_, col, SHADER_UNIFORM_VEC4);
    SetShaderValue(lit_, loc_fog_rng_, rng, SHADER_UNIFORM_VEC2);
}

void AssetManager::apply_lit(Model& m) const noexcept {
    if (lit_.id == 0) return;
    for (int i = 0; i < m.materialCount; ++i) m.materials[i].shader = lit_;
}

// The lit shader is shared, so detach it before UnloadModel() (which would
// otherwise unload it once per material).
void AssetManager::release(Model& m) const noexcept {
    if (m.meshCount == 0) return;
    for (int i = 0; i < m.materialCount; ++i)
        if (m.materials[i].shader.id == lit_.id && lit_.id != 0) {
            m.materials[i].shader.id   = rlGetShaderIdDefault();
            m.materials[i].shader.locs = rlGetShaderLocsDefault();
        }
    UnloadModel(m);
    m = Model{};
}

void AssetManager::release(ModelEntry& e) const noexcept {
    release(e.model);
    for (Spinner& sp : e.spinners) release(sp.model);
    e.spinners.clear();
}

AssetManager::AssetManager()  {}

AssetManager::~AssetManager() { stop_worker(); unload_all(); }

void AssetManager::unload_all() noexcept {
    for (auto& [k, e] : named_)      release(e);
    for (auto& [k, e] : procedural_) release(e);
    if (ready_) release(fallback_);
    named_.clear();
    procedural_.clear();
    if (lit_.id != 0) { UnloadShader(lit_); lit_ = Shader{}; }
    ready_ = false;
}

bool AssetManager::load(const std::string& name,
                         const std::filesystem::path& path,
                         float import_scale,
                         Color tint,
                         Quaternion base_rot) noexcept
{
    if (named_.contains(name)) return true;
    ModelEntry e;
    e.scale = import_scale;
    if (!convert_assimp(path, import_scale, tint, base_rot, e)) return false;
    named_.emplace(name, std::move(e));
    return true;
}

// ── Async loading ─────────────────────────────────────────────────────────────
void AssetManager::stop_worker() noexcept {
    worker_run_.store(false);
    jobs_cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void AssetManager::request_load(const std::string& name,
                                const std::filesystem::path& path,
                                uint16_t type, float import_scale,
                                Color tint, Quaternion base_rot) noexcept
{
    if (named_.contains(name)) { map_type(type, name); return; }
    if (!worker_run_.exchange(true))
        worker_ = std::thread(&AssetManager::worker_loop, this);
    {
        std::lock_guard lk(jobs_mu_);
        jobs_.push_back({name, path, type, import_scale, tint, base_rot});
    }
    inflight_.fetch_add(1);
    jobs_cv_.notify_one();
}

void AssetManager::worker_loop() {
    while (worker_run_.load()) {
        LoadJob job;
        {
            std::unique_lock lk(jobs_mu_);
            jobs_cv_.wait(lk, [&]{ return !jobs_.empty() || !worker_run_.load(); });
            if (!worker_run_.load()) break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        ReadyModel rm; rm.job = job;
        rm.ok = parse_file(job.path, rm.cpu);   // NO GL here — worker-safe
        {
            std::lock_guard lk(ready_mu_);
            ready_models_.push_back(std::move(rm));
        }
    }
}

std::vector<uint16_t> AssetManager::pump_uploads(int budget) noexcept {
    std::vector<uint16_t> updated;
    for (int i = 0; i < budget; ++i) {
        ReadyModel rm;
        {
            std::lock_guard lk(ready_mu_);
            if (ready_models_.empty()) break;
            rm = std::move(ready_models_.back());
            ready_models_.pop_back();
        }
        inflight_.fetch_sub(1);
        if (!rm.ok) {
            TraceLog(LOG_WARNING, "AssetManager: failed to parse '%s'",
                     rm.job.path.string().c_str());
            continue;
        }
        ModelEntry e;
        if (upload_cpu(rm.cpu, rm.job.scale, rm.job.tint, rm.job.base_rot, e)) {
            named_[rm.job.name] = std::move(e);   // GPU upload on the main thread
            map_type(rm.job.type, rm.job.name);
            updated.push_back(rm.job.type);
            TraceLog(LOG_INFO, "AssetManager: model ready for type %u ('%s')",
                     rm.job.type, rm.job.name.c_str());
        }
    }
    return updated;
}

void AssetManager::map_type(uint16_t type_id, const std::string& name) {
    type_to_name_[type_id] = name;
}

void AssetManager::unmap_type(uint16_t type_id) {
    type_to_name_.erase(type_id);
}

const ModelEntry* AssetManager::get_for_type(uint16_t type_id) const noexcept {
    // 1. Loaded model override?
    if (auto it = type_to_name_.find(type_id); it != type_to_name_.end()) {
        if (auto m = named_.find(it->second); m != named_.end())
            return &m->second;
    }
    // 2. Built-in procedural shape?
    if (auto p = procedural_.find(type_id); p != procedural_.end())
        return &p->second;
    // 3. Fallback cube
    return &fallback_;
}

// ── Assimp → Raylib conversion ────────────────────────────────────────────────
// Assimp parse + CPU vertex assembly. NO raylib/GL calls — safe on any thread.
bool AssetManager::parse_file(const std::filesystem::path& path, CpuMesh& out) noexcept
{
    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, 1.0f);

    const aiScene* scene = importer.ReadFile(path.string(),
        aiProcess_Triangulate | aiProcess_GenSmoothNormals |
        aiProcess_JoinIdenticalVertices | aiProcess_OptimizeMeshes |
        aiProcess_FlipUVs | aiProcess_GlobalScale);

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        TraceLog(LOG_WARNING, "AssetManager: '%s': %s",
                 path.filename().string().c_str(), importer.GetErrorString());
        return false;
    }

    unsigned short vert_offset = 0;
    for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* m = scene->mMeshes[mi];
        if (!m->HasPositions()) continue;

        for (uint32_t vi = 0; vi < m->mNumVertices; ++vi) {
            out.vertices.push_back(m->mVertices[vi].x);
            out.vertices.push_back(m->mVertices[vi].y);
            out.vertices.push_back(m->mVertices[vi].z);
            if (m->HasNormals()) {
                out.normals.push_back(m->mNormals[vi].x);
                out.normals.push_back(m->mNormals[vi].y);
                out.normals.push_back(m->mNormals[vi].z);
            } else { out.normals.push_back(0); out.normals.push_back(1); out.normals.push_back(0); }
            if (m->HasTextureCoords(0)) {
                out.texcoords.push_back(m->mTextureCoords[0][vi].x);
                out.texcoords.push_back(m->mTextureCoords[0][vi].y);
            } else { out.texcoords.push_back(0); out.texcoords.push_back(0); }
        }
        for (uint32_t fi = 0; fi < m->mNumFaces; ++fi) {
            const aiFace& f = m->mFaces[fi];
            if (f.mNumIndices != 3) continue;
            out.indices.push_back(static_cast<unsigned short>(vert_offset + f.mIndices[0]));
            out.indices.push_back(static_cast<unsigned short>(vert_offset + f.mIndices[1]));
            out.indices.push_back(static_cast<unsigned short>(vert_offset + f.mIndices[2]));
        }
        vert_offset += static_cast<unsigned short>(m->mNumVertices);
    }
    return !out.vertices.empty();
}

// GPU upload of a parsed CpuMesh — MAIN THREAD ONLY (raylib GL calls).
bool AssetManager::upload_cpu(const CpuMesh& cpu, float scale, Color tint,
                              Quaternion base_rot, ModelEntry& out) noexcept
{
    if (cpu.vertices.empty()) return false;
    Mesh mesh{};
    mesh.vertexCount   = static_cast<int>(cpu.vertices.size() / 3);
    mesh.triangleCount = static_cast<int>(cpu.indices.size()  / 3);
    mesh.vertices  = (float*)MemAlloc((unsigned)(cpu.vertices.size()  * sizeof(float)));
    mesh.normals   = (float*)MemAlloc((unsigned)(cpu.normals.size()   * sizeof(float)));
    mesh.texcoords = (float*)MemAlloc((unsigned)(cpu.texcoords.size() * sizeof(float)));
    mesh.indices   = (unsigned short*)MemAlloc((unsigned)(cpu.indices.size() * sizeof(unsigned short)));
    std::memcpy(mesh.vertices,  cpu.vertices.data(),  cpu.vertices.size()  * sizeof(float));
    std::memcpy(mesh.normals,   cpu.normals.data(),   cpu.normals.size()   * sizeof(float));
    std::memcpy(mesh.texcoords, cpu.texcoords.data(), cpu.texcoords.size() * sizeof(float));
    std::memcpy(mesh.indices,   cpu.indices.data(),   cpu.indices.size()   * sizeof(unsigned short));
    UploadMesh(&mesh, false);

    out.model    = LoadModelFromMesh(mesh);
    out.tint     = tint;
    out.scale    = scale;
    out.base_rot = base_rot;
    out.length   = model_extent(out.model) * scale;
    apply_lit(out.model);
    return true;
}

// Synchronous import = parse + upload on the calling (main) thread.
bool AssetManager::convert_assimp(const std::filesystem::path& path,
                                   float import_scale, Color tint,
                                   Quaternion base_rot, ModelEntry& out) noexcept
{
    CpuMesh cpu;
    if (!parse_file(path, cpu)) return false;
    if (!upload_cpu(cpu, import_scale, tint, base_rot, out)) return false;
    TraceLog(LOG_INFO, "AssetManager: loaded '%s' (%d verts)",
             path.filename().string().c_str(), out.model.meshCount ? out.model.meshes[0].vertexCount : 0);
    return true;
}

} // namespace afteraction
