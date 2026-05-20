#include "AssetManager.hpp"
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <raymath.h>
#include <rlgl.h>
#include <cstring>
#include <vector>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace debrief {

// 90° around X: makes a Y-up cone / cylinder point forward (-Z)
static Quaternion rot90x() {
    return QuaternionFromAxisAngle({1,0,0}, -(float)M_PI * 0.5f);
}

// ── Procedural default shapes ─────────────────────────────────────────────────
//
//  All GenMesh primitives are Y-axis-aligned by default.
//  base_rot = RotX(-90°) reorients them so the "tip" or "nose" points
//  toward -Z (North), matching our heading convention (psi=0 → -Z).
//
//  Sizes are in metres — adjust scale in RenderModel if needed.
void AssetManager::init_procedural() {
    Quaternion fwd = rot90x(); // tip toward -Z

    // TYPE_JET  (1) — elongated cone, blue
    {
        ModelEntry e;
        e.model    = LoadModelFromMesh(GenMeshCone(6.0f, 28.0f, 8));
        e.tint     = { 80, 140, 255, 255 };
        e.scale    = 1.0f;
        e.base_rot = fwd;
        procedural_[net::TYPE_JET] = std::move(e);
    }
    // TYPE_MISSILE (2) — slim cylinder, red
    {
        ModelEntry e;
        e.model    = LoadModelFromMesh(GenMeshCylinder(1.5f, 18.0f, 6));
        e.tint     = { 255, 60, 60, 255 };
        e.scale    = 1.0f;
        e.base_rot = fwd;
        procedural_[net::TYPE_MISSILE] = std::move(e);
    }
    // TYPE_AAA (3) — short wide cylinder, dark
    {
        ModelEntry e;
        e.model    = LoadModelFromMesh(GenMeshCylinder(4.0f, 6.0f, 8));
        e.tint     = { 60, 70, 60, 255 };
        e.scale    = 1.0f;
        e.base_rot = {0,0,0,1};  // stays upright — barrel points Y-up
        procedural_[net::TYPE_AAA] = std::move(e);
    }
    // TYPE_GROUND (4) — flat box, orange
    {
        ModelEntry e;
        e.model    = LoadModelFromMesh(GenMeshCube(14.0f, 3.0f, 22.0f));
        e.tint     = { 220, 120, 30, 255 };
        e.scale    = 1.0f;
        e.base_rot = {0,0,0,1};
        procedural_[net::TYPE_GROUND] = std::move(e);
    }
    // TYPE_HELO (5) — sphere body, green
    {
        ModelEntry e;
        e.model    = LoadModelFromMesh(GenMeshSphere(7.0f, 10, 10));
        e.tint     = { 60, 200, 80, 255 };
        e.scale    = 1.0f;
        e.base_rot = {0,0,0,1};
        procedural_[net::TYPE_HELO] = std::move(e);
    }
    // TYPE_SHIP (6) — long flat box, grey
    {
        ModelEntry e;
        e.model    = LoadModelFromMesh(GenMeshCube(18.0f, 5.0f, 60.0f));
        e.tint     = { 120, 120, 140, 255 };
        e.scale    = 1.0f;
        e.base_rot = fwd;
        procedural_[net::TYPE_SHIP] = std::move(e);
    }

    // Fallback cube for unknown types
    fallback_.model    = LoadModelFromMesh(GenMeshCube(8.0f, 8.0f, 8.0f));
    fallback_.tint     = WHITE;
    fallback_.base_rot = {0,0,0,1};
    ready_ = true;
}

void AssetManager::init()     { init_procedural(); }

AssetManager::~AssetManager() { unload_all(); }

void AssetManager::unload_all() noexcept {
    for (auto& [k, e] : named_)      UnloadModel(e.model);
    for (auto& [k, e] : procedural_) UnloadModel(e.model);
    if (ready_) UnloadModel(fallback_.model);
    named_.clear();
    procedural_.clear();
    ready_ = false;
}

bool AssetManager::load(const std::string& name,
                         const std::filesystem::path& path,
                         float import_scale) noexcept
{
    if (named_.contains(name)) return true;
    ModelEntry e;
    e.scale = import_scale;
    if (!convert_assimp(path, import_scale, e)) return false;
    named_.emplace(name, std::move(e));
    return true;
}

void AssetManager::map_type(uint16_t type_id, const std::string& name) {
    type_to_name_[type_id] = name;
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
bool AssetManager::convert_assimp(const std::filesystem::path& path,
                                   float import_scale, ModelEntry& out) noexcept
{
    Assimp::Importer importer;
    importer.SetPropertyFloat(AI_CONFIG_GLOBAL_SCALE_FACTOR_KEY, import_scale);

    const aiScene* scene = importer.ReadFile(path.string(),
        aiProcess_Triangulate | aiProcess_GenSmoothNormals |
        aiProcess_JoinIdenticalVertices | aiProcess_OptimizeMeshes |
        aiProcess_FlipUVs | aiProcess_GlobalScale);

    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || !scene->mRootNode) {
        TraceLog(LOG_WARNING, "AssetManager: '%s': %s",
                 path.filename().string().c_str(), importer.GetErrorString());
        return false;
    }

    std::vector<float>          vertices, normals, texcoords;
    std::vector<unsigned short> indices;
    unsigned short vert_offset = 0;

    for (uint32_t mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* m = scene->mMeshes[mi];
        if (!m->HasPositions()) continue;

        for (uint32_t vi = 0; vi < m->mNumVertices; ++vi) {
            vertices.push_back(m->mVertices[vi].x);
            vertices.push_back(m->mVertices[vi].y);
            vertices.push_back(m->mVertices[vi].z);
            if (m->HasNormals()) {
                normals.push_back(m->mNormals[vi].x);
                normals.push_back(m->mNormals[vi].y);
                normals.push_back(m->mNormals[vi].z);
            } else { normals.push_back(0); normals.push_back(1); normals.push_back(0); }
            if (m->HasTextureCoords(0)) {
                texcoords.push_back(m->mTextureCoords[0][vi].x);
                texcoords.push_back(m->mTextureCoords[0][vi].y);
            } else { texcoords.push_back(0); texcoords.push_back(0); }
        }
        for (uint32_t fi = 0; fi < m->mNumFaces; ++fi) {
            const aiFace& f = m->mFaces[fi];
            if (f.mNumIndices != 3) continue;
            indices.push_back(static_cast<unsigned short>(vert_offset + f.mIndices[0]));
            indices.push_back(static_cast<unsigned short>(vert_offset + f.mIndices[1]));
            indices.push_back(static_cast<unsigned short>(vert_offset + f.mIndices[2]));
        }
        vert_offset += static_cast<unsigned short>(m->mNumVertices);
    }

    if (vertices.empty()) return false;

    Mesh mesh{};
    mesh.vertexCount   = static_cast<int>(vertices.size() / 3);
    mesh.triangleCount = static_cast<int>(indices.size()  / 3);
    mesh.vertices  = (float*)MemAlloc((unsigned)(vertices.size()  * sizeof(float)));
    mesh.normals   = (float*)MemAlloc((unsigned)(normals.size()   * sizeof(float)));
    mesh.texcoords = (float*)MemAlloc((unsigned)(texcoords.size() * sizeof(float)));
    mesh.indices   = (unsigned short*)MemAlloc((unsigned)(indices.size() * sizeof(unsigned short)));
    std::memcpy(mesh.vertices,  vertices.data(),  vertices.size()  * sizeof(float));
    std::memcpy(mesh.normals,   normals.data(),   normals.size()   * sizeof(float));
    std::memcpy(mesh.texcoords, texcoords.data(), texcoords.size() * sizeof(float));
    std::memcpy(mesh.indices,   indices.data(),   indices.size()   * sizeof(unsigned short));
    UploadMesh(&mesh, false);

    out.model    = LoadModelFromMesh(mesh);
    out.tint     = WHITE;
    out.base_rot = {0,0,0,1};

    TraceLog(LOG_INFO, "AssetManager: loaded '%s' (%d verts)",
             path.filename().string().c_str(), mesh.vertexCount);
    return true;
}

} // namespace debrief
