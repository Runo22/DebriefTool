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

namespace afteraction {

// Helper to merge multiple temporary meshes with local transforms into a single master Raylib Mesh.
static Mesh merge_meshes(const std::vector<std::pair<Mesh, Matrix>>& parts) {
    std::vector<float> vertices;
    std::vector<float> normals;
    std::vector<float> texcoords;
    std::vector<unsigned short> indices;
    
    for (const auto& [mesh, transform] : parts) {
        unsigned short vert_offset = static_cast<unsigned short>(vertices.size() / 3);
        
        // Normal transformation matrix (rotation only, translation zeroed out)
        Matrix rot_only = transform;
        rot_only.m12 = 0.0f; rot_only.m13 = 0.0f; rot_only.m14 = 0.0f;
        
        for (int i = 0; i < mesh.vertexCount; ++i) {
            Vector3 v = { mesh.vertices[i*3], mesh.vertices[i*3+1], mesh.vertices[i*3+2] };
            Vector3 vt = Vector3Transform(v, transform);
            vertices.push_back(vt.x);
            vertices.push_back(vt.y);
            vertices.push_back(vt.z);
            
            if (mesh.normals) {
                Vector3 n = { mesh.normals[i*3], mesh.normals[i*3+1], mesh.normals[i*3+2] };
                Vector3 nt = Vector3Normalize(Vector3Transform(n, rot_only));
                normals.push_back(nt.x);
                normals.push_back(nt.y);
                normals.push_back(nt.z);
            } else {
                normals.push_back(0.0f);
                normals.push_back(1.0f);
                normals.push_back(0.0f);
            }
            
            if (mesh.texcoords) {
                texcoords.push_back(mesh.texcoords[i*2]);
                texcoords.push_back(mesh.texcoords[i*2+1]);
            } else {
                texcoords.push_back(0.0f);
                texcoords.push_back(0.0f);
            }
        }
        
        if (mesh.indices) {
            for (int i = 0; i < mesh.triangleCount * 3; ++i) {
                indices.push_back(static_cast<unsigned short>(vert_offset + mesh.indices[i]));
            }
        } else {
            for (int i = 0; i < mesh.vertexCount; ++i) {
                indices.push_back(static_cast<unsigned short>(vert_offset + i));
            }
        }
    }
    
    Mesh out_mesh{};
    if (!vertices.empty()) {
        out_mesh.vertexCount   = static_cast<int>(vertices.size() / 3);
        out_mesh.triangleCount = static_cast<int>(indices.size()  / 3);
        out_mesh.vertices  = (float*)MemAlloc((unsigned)(vertices.size()  * sizeof(float)));
        out_mesh.normals   = (float*)MemAlloc((unsigned)(normals.size()   * sizeof(float)));
        out_mesh.texcoords = (float*)MemAlloc((unsigned)(texcoords.size() * sizeof(float)));
        out_mesh.indices   = (unsigned short*)MemAlloc((unsigned)(indices.size() * sizeof(unsigned short)));
        std::memcpy(out_mesh.vertices,  vertices.data(),  vertices.size()  * sizeof(float));
        std::memcpy(out_mesh.normals,   normals.data(),   normals.size()   * sizeof(float));
        std::memcpy(out_mesh.texcoords, texcoords.data(), texcoords.size() * sizeof(float));
        std::memcpy(out_mesh.indices,   indices.data(),   indices.size()   * sizeof(unsigned short));
        UploadMesh(&out_mesh, false);
    }
    return out_mesh;
}

// ── Procedural default shapes ─────────────────────────────────────────────────
//
//  All composite procedural models are natively aligned so:
//    - Nose / Front points to -Z (Forward)
//    - Wings / Width lies along X (Right)
//    - Top points to +Y (Up)
//  This allows them to use base_rot = {0,0,0,1} (identity rotation) and
//  align perfectly with the flight simulation heading/pitch/roll logic!
void AssetManager::init_procedural() {
    // TYPE_JET (1) — Detailed high-tech fighter jet, blue-cyan tint
    {
        std::vector<std::pair<Mesh, Matrix>> parts;
        Mesh fuse = GenMeshCylinder(1.2f, 15.0f, 8);
        parts.push_back({fuse, MatrixMultiply(MatrixRotateX(-M_PI/2.0f), MatrixTranslate(0.0f, 0.0f, 2.5f))});
        
        Mesh nose = GenMeshCone(1.2f, 10.0f, 8);
        parts.push_back({nose, MatrixMultiply(MatrixRotateX(-M_PI/2.0f), MatrixTranslate(0.0f, 0.0f, -10.0f))});
        
        Mesh canopy = GenMeshSphere(0.9f, 8, 8);
        parts.push_back({canopy, MatrixTranslate(0.0f, 1.0f, -3.0f)});
        
        Mesh wings = GenMeshCube(16.0f, 0.15f, 5.0f);
        parts.push_back({wings, MatrixTranslate(0.0f, 0.0f, 2.0f)});
        
        Mesh tail_fin = GenMeshCube(0.2f, 4.0f, 3.0f);
        parts.push_back({tail_fin, MatrixTranslate(0.0f, 2.0f, 8.5f)});
        
        Mesh tail_stab = GenMeshCube(6.0f, 0.1f, 2.5f);
        parts.push_back({tail_stab, MatrixTranslate(0.0f, 0.0f, 8.5f)});
        
        procedural_[net::TYPE_JET] = { LoadModelFromMesh(merge_meshes(parts)), { 80, 140, 255, 255 }, 1.0f, {0,0,0,1} };
        
        UnloadMesh(fuse);
        UnloadMesh(nose);
        UnloadMesh(canopy);
        UnloadMesh(wings);
        UnloadMesh(tail_fin);
        UnloadMesh(tail_stab);
    }

    // TYPE_MISSILE (2) — Sleek tactical missile, crimson red tint
    {
        std::vector<std::pair<Mesh, Matrix>> parts;
        Mesh body = GenMeshCylinder(0.35f, 6.0f, 6);
        parts.push_back({body, MatrixMultiply(MatrixRotateX(-M_PI/2.0f), MatrixTranslate(0.0f, 0.0f, 1.0f))});
        
        Mesh nose = GenMeshCone(0.35f, 4.0f, 6);
        parts.push_back({nose, MatrixMultiply(MatrixRotateX(-M_PI/2.0f), MatrixTranslate(0.0f, 0.0f, -4.0f))});
        
        Mesh fin1 = GenMeshCube(0.05f, 2.2f, 0.8f);
        parts.push_back({fin1, MatrixTranslate(0.0f, 0.0f, 3.5f)});
        
        Mesh fin2 = GenMeshCube(2.2f, 0.05f, 0.8f);
        parts.push_back({fin2, MatrixTranslate(0.0f, 0.0f, 3.5f)});
        
        procedural_[net::TYPE_MISSILE] = { LoadModelFromMesh(merge_meshes(parts)), { 255, 60, 60, 255 }, 1.0f, {0,0,0,1} };
        
        UnloadMesh(body);
        UnloadMesh(nose);
        UnloadMesh(fin1);
        UnloadMesh(fin2);
    }

    // TYPE_AAA (3) — Dual-barrel mobile air-defense vehicle, dark forest-green tint
    {
        std::vector<std::pair<Mesh, Matrix>> parts;
        Mesh base = GenMeshCube(7.0f, 2.0f, 9.0f);
        parts.push_back({base, MatrixTranslate(0.0f, 1.0f, 0.0f)});
        
        Mesh turret = GenMeshCylinder(2.5f, 2.0f, 8);
        parts.push_back({turret, MatrixTranslate(0.0f, 3.0f, 0.0f)});
        
        Mesh radar = GenMeshCube(2.5f, 1.2f, 0.8f);
        parts.push_back({radar, MatrixTranslate(0.0f, 4.0f, 2.0f)});
        
        Mesh bar1 = GenMeshCylinder(0.2f, 6.0f, 6);
        parts.push_back({bar1, MatrixMultiply(MatrixRotateX(-M_PI/2.0f), MatrixTranslate(-1.2f, 3.2f, -3.5f))});
        
        Mesh bar2 = GenMeshCylinder(0.2f, 6.0f, 6);
        parts.push_back({bar2, MatrixMultiply(MatrixRotateX(-M_PI/2.0f), MatrixTranslate(1.2f, 3.2f, -3.5f))});
        
        procedural_[net::TYPE_AAA] = { LoadModelFromMesh(merge_meshes(parts)), { 60, 75, 60, 255 }, 1.0f, {0,0,0,1} };
        
        UnloadMesh(base);
        UnloadMesh(turret);
        UnloadMesh(radar);
        UnloadMesh(bar1);
        UnloadMesh(bar2);
    }

    // TYPE_GROUND (4) — 6x6 Armored Command Vehicle, sand orange-brown tint
    {
        std::vector<std::pair<Mesh, Matrix>> parts;
        Mesh hull = GenMeshCube(6.0f, 2.4f, 10.0f);
        parts.push_back({hull, MatrixTranslate(0.0f, 1.8f, 0.0f)});
        
        Mesh cab = GenMeshCube(5.0f, 1.2f, 6.0f);
        parts.push_back({cab, MatrixTranslate(0.0f, 3.6f, -1.0f)});
        
        Mesh wheel = GenMeshCylinder(1.2f, 0.8f, 8);
        for (float z : {-3.5f, 0.0f, 3.5f}) {
            parts.push_back({wheel, MatrixMultiply(MatrixRotateZ(M_PI/2.0f), MatrixTranslate(-3.1f, 0.6f, z))});
            parts.push_back({wheel, MatrixMultiply(MatrixRotateZ(M_PI/2.0f), MatrixTranslate(3.1f, 0.6f, z))});
        }
        
        procedural_[net::TYPE_GROUND] = { LoadModelFromMesh(merge_meshes(parts)), { 220, 120, 30, 255 }, 1.0f, {0,0,0,1} };
        
        UnloadMesh(hull);
        UnloadMesh(cab);
        UnloadMesh(wheel);
    }

    // TYPE_HELO (5) — Tactical combat helicopter, olive green tint
    {
        std::vector<std::pair<Mesh, Matrix>> parts;
        Mesh cabin = GenMeshSphere(2.5f, 8, 8);
        parts.push_back({cabin, MatrixTranslate(0.0f, 0.0f, -2.0f)});
        
        Mesh boom = GenMeshCylinder(0.5f, 8.0f, 6);
        parts.push_back({boom, MatrixMultiply(MatrixRotateX(-M_PI/2.0f), MatrixTranslate(0.0f, 0.0f, 4.0f))});
        
        Mesh shaft = GenMeshCylinder(0.15f, 1.8f, 6);
        parts.push_back({shaft, MatrixTranslate(0.0f, 2.0f, -2.0f)});
        
        Mesh rotor = GenMeshCube(16.0f, 0.05f, 0.8f);
        parts.push_back({rotor, MatrixTranslate(0.0f, 2.9f, -2.0f)});
        
        Mesh tail = GenMeshCylinder(1.2f, 0.1f, 6);
        parts.push_back({tail, MatrixMultiply(MatrixRotateZ(M_PI/2.0f), MatrixTranslate(0.6f, 1.5f, 8.0f))});
        
        procedural_[net::TYPE_HELO] = { LoadModelFromMesh(merge_meshes(parts)), { 60, 200, 80, 255 }, 1.0f, {0,0,0,1} };
        
        UnloadMesh(cabin);
        UnloadMesh(boom);
        UnloadMesh(shaft);
        UnloadMesh(rotor);
        UnloadMesh(tail);
    }

    // TYPE_SHIP (6) — Guided-missile naval destroyer, deck grey tint
    {
        std::vector<std::pair<Mesh, Matrix>> parts;
        Mesh hull = GenMeshCube(10.0f, 4.0f, 45.0f);
        parts.push_back({hull, MatrixTranslate(0.0f, 2.0f, 5.0f)});
        
        Mesh bow = GenMeshCone(5.0f, 15.0f, 8);
        parts.push_back({bow, MatrixMultiply(MatrixRotateX(-M_PI/2.0f), MatrixTranslate(0.0f, 2.0f, -25.0f))});
        
        Mesh bridge = GenMeshCube(7.0f, 3.0f, 12.0f);
        parts.push_back({bridge, MatrixTranslate(0.0f, 5.5f, -5.0f)});
        
        Mesh mast = GenMeshCylinder(0.4f, 4.0f, 6);
        parts.push_back({mast, MatrixTranslate(0.0f, 9.0f, -5.0f)});
        
        Mesh aft = GenMeshCube(8.0f, 2.5f, 15.0f);
        parts.push_back({aft, MatrixTranslate(0.0f, 5.25f, 12.0f)});
        
        Mesh gun = GenMeshCube(3.0f, 1.5f, 4.0f);
        parts.push_back({gun, MatrixTranslate(0.0f, 4.75f, -12.0f)});
        
        Mesh barrel = GenMeshCylinder(0.15f, 6.0f, 6);
        parts.push_back({barrel, MatrixMultiply(MatrixRotateX(-M_PI/2.0f), MatrixTranslate(0.0f, 5.2f, -17.0f))});
        
        procedural_[net::TYPE_SHIP] = { LoadModelFromMesh(merge_meshes(parts)), { 120, 120, 140, 255 }, 1.0f, {0,0,0,1} };
        
        UnloadMesh(hull);
        UnloadMesh(bow);
        UnloadMesh(bridge);
        UnloadMesh(mast);
        UnloadMesh(aft);
        UnloadMesh(gun);
        UnloadMesh(barrel);
    }

    // Fallback high-tech chevron / generic drone model for unknown/default entities
    {
        std::vector<std::pair<Mesh, Matrix>> parts;
        // Central body cone
        Mesh body = GenMeshCone(1.0f, 6.0f, 6);
        parts.push_back({body, MatrixMultiply(MatrixRotateX(-M_PI/2.0f), MatrixTranslate(0.0f, 0.0f, -1.0f))});
        // Swept-back chevron wings
        Mesh wings = GenMeshCube(8.0f, 0.2f, 2.5f);
        parts.push_back({wings, MatrixTranslate(0.0f, 0.0f, 1.0f)});
        // Small tail stabilizer
        Mesh tail = GenMeshCube(0.2f, 2.0f, 1.5f);
        parts.push_back({tail, MatrixTranslate(0.0f, 1.0f, 2.5f)});

        fallback_.model = LoadModelFromMesh(merge_meshes(parts));
        fallback_.tint  = { 180, 200, 220, 255 }; // High-tech light steel blue
        fallback_.scale = 1.0f;
        fallback_.base_rot = {0,0,0,1};

        UnloadMesh(body);
        UnloadMesh(wings);
        UnloadMesh(tail);
    }
    ready_ = true;
}

AssetManager::AssetManager()  {}

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

} // namespace afteraction
