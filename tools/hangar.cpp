// ─────────────────────────────────────────────────────────────────────────────
//  AfterAction Hangar — standalone model preview
//
//  Loads an FBX / OBJ / glTF-GLB through the SAME AssetManager pipeline that
//  AfterAction uses, and renders it with the SAME DrawModelEx + base_rot math.
//  So what you see here is what you get in the app — use it to dial in the
//  scale and base-rotation, then copy the generated snippet into
//  assets/models.yaml.
//
//  Usage:  hangar <model.fbx|obj|glb> [scale]
// ─────────────────────────────────────────────────────────────────────────────
#include "render/AssetManager.hpp"
#include <raylib.h>
#include <raymath.h>
#include <rlImGui.h>
#include <imgui.h>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace afteraction;

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("AfterAction Hangar — model preview\n");
        printf("Usage: hangar <model.fbx|obj|glb> [scale]\n");
        return 1;
    }
    const std::string path = argv[1];
    float ui_scale = (argc >= 3) ? (float)atof(argv[2]) : 1.0f;

    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE);
    InitWindow(1280, 800, "AfterAction Hangar — Model Preview");
    SetTargetFPS(60);
    rlImGuiSetup(true);
    ImGui::GetIO().IniFilename = nullptr;

    // Same import path as the app (synchronous is fine for a preview tool).
    AssetManager assets;
    const bool ok = assets.load("preview", path, 1.0f);
    if (ok) assets.map_type(0, "preview");
    const ModelEntry* me = ok ? assets.get_for_type(0) : nullptr;

    Vector3 dim{1,1,1};
    float model_radius = 1.0f;
    if (ok) {
        BoundingBox bb = GetModelBoundingBox(me->model);
        dim = { bb.max.x - bb.min.x, bb.max.y - bb.min.y, bb.max.z - bb.min.z };
        model_radius = Vector3Length(dim) * 0.5f;
        if (model_radius < 1e-3f) model_radius = 1.0f;
    }

    Camera3D cam{};
    cam.target     = { 0, dim.y * 0.25f, 0 };
    cam.up         = { 0, 1, 0 };
    cam.fovy       = 50.0f;
    cam.projection = CAMERA_PERSPECTIVE;
    float cam_dist  = model_radius * 3.0f;
    float orbit_yaw = 35.0f, orbit_pitch = 25.0f;

    // Preview controls — mirror the manifest fields.
    float yaw = 0, pitch = 0, roll = 0;   // base_rot, degrees
    float tint[3] = {1, 1, 1};
    bool  wire = true, spin = true, grid = true, axes = true;
    const float kSceneScale = 30.0f;      // AfterAction's default Scene Scale

    while (!WindowShouldClose()) {
        if (!ImGui::GetIO().WantCaptureMouse) {
            if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
                Vector2 d = GetMouseDelta();
                orbit_yaw   -= d.x * 0.3f;
                orbit_pitch  = Clamp(orbit_pitch - d.y * 0.3f, -89.0f, 89.0f);
            }
            float w = GetMouseWheelMove();
            if (w != 0) cam_dist = Clamp(cam_dist * powf(0.85f, w),
                                         model_radius * 0.3f, model_radius * 30.0f);
        }

        float oy = orbit_yaw * DEG2RAD, op = orbit_pitch * DEG2RAD;
        cam.position = Vector3Add(cam.target, {
            cam_dist * cosf(op) * sinf(oy),
            cam_dist * sinf(op),
            cam_dist * cosf(op) * cosf(oy) });

        // Heading spin stands in for the sim's psi (entity orientation about Y).
        float spin_deg = spin ? (float)GetTime() * 30.0f : 0.0f;

        // base_rot — IDENTICAL conversion to the manifest loader.
        Quaternion base_rot = QuaternionFromEuler(pitch * DEG2RAD, yaw * DEG2RAD, roll * DEG2RAD);
        Quaternion ent      = QuaternionFromAxisAngle({0,1,0}, spin_deg * DEG2RAD);
        Quaternion q        = QuaternionMultiply(ent, base_rot);   // EXACT app order
        Vector3 axis{0,1,0}; float ang = 0;
        QuaternionToAxisAngle(q, &axis, &ang);
        Color col = { (unsigned char)(tint[0]*255), (unsigned char)(tint[1]*255),
                      (unsigned char)(tint[2]*255), 255 };

        BeginDrawing();
        ClearBackground({18, 22, 30, 255});
        BeginMode3D(cam);
        if (grid) DrawGrid(20, model_radius * 0.5f);
        if (axes) {
            float L = model_radius * 1.4f;
            DrawLine3D({0,0,0}, {0,0,-L}, {0,200,255,255});   // forward = -Z (cyan)
            DrawLine3D({0,0,0}, {0, L,0}, {0,255,120,255});   // up      = +Y (green)
            DrawLine3D({0,0,0}, {L,0,0}, {255,120,120,255});  // right   = +X (red)
        }
        if (me) {
            DrawModelEx(me->model, {0,0,0}, axis, ang * RAD2DEG, {ui_scale,ui_scale,ui_scale}, col);
            if (wire)
                DrawModelWiresEx(me->model, {0,0,0}, axis, ang * RAD2DEG,
                                 {ui_scale,ui_scale,ui_scale}, ColorAlpha(col, 0.35f));
        }
        EndMode3D();

        rlImGuiBegin();
        ImGui::SetNextWindowPos({10,10}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({340, 470}, ImGuiCond_FirstUseEver);
        ImGui::Begin("Model Preview");
        if (!ok) {
            ImGui::TextColored({1,0.4f,0.3f,1}, "FAILED to load:");
            ImGui::TextWrapped("%s", path.c_str());
            ImGui::TextDisabled("Check the path and that it's FBX/OBJ/glTF.");
        } else {
            ImGui::Text("File:  %s", GetFileName(path.c_str()));
            ImGui::Text("Verts: %d", me->model.meshCount ? me->model.meshes[0].vertexCount : 0);
            ImGui::Text("Size (model units): %.2f x %.2f x %.2f", dim.x, dim.y, dim.z);
            ImGui::Separator();
            ImGui::TextDisabled("Base rotation — align the nose to the CYAN forward axis (-Z):");
            ImGui::SliderFloat("Yaw",   &yaw,   -180, 180, "%.0f deg");
            ImGui::SliderFloat("Pitch", &pitch, -180, 180, "%.0f deg");
            ImGui::SliderFloat("Roll",  &roll,  -180, 180, "%.0f deg");
            if (ImGui::Button("Reset rotation")) { yaw = pitch = roll = 0; }
            ImGui::Separator();
            ImGui::SliderFloat("Scale", &ui_scale, 0.001f, 50.0f, "%.3f",
                               ImGuiSliderFlags_Logarithmic);
            ImGui::TextDisabled("In-app size = scale x Scene Scale (~%.0f)", kSceneScale);
            ImGui::ColorEdit3("Tint", tint);
            ImGui::Checkbox("Wire", &wire); ImGui::SameLine();
            ImGui::Checkbox("Spin", &spin); ImGui::SameLine();
            ImGui::Checkbox("Grid", &grid); ImGui::SameLine();
            ImGui::Checkbox("Axes", &axes);
            ImGui::Separator();
            ImGui::TextDisabled("Copy into assets/models.yaml:");
            char snip[320];
            snprintf(snip, sizeof(snip),
                "  - type: 1\n    file: models/%s\n    scale: %.3f\n"
                "    tint: [%d, %d, %d]\n    base_rot: [%.0f, %.0f, %.0f]",
                GetFileName(path.c_str()), ui_scale,
                (int)(tint[0]*255), (int)(tint[1]*255), (int)(tint[2]*255),
                yaw, pitch, roll);
            ImGui::InputTextMultiline("##snip", snip, sizeof(snip), {-1, 96},
                                      ImGuiInputTextFlags_ReadOnly);
        }
        ImGui::Separator();
        ImGui::TextDisabled("RMB drag: orbit   Wheel: zoom");
        ImGui::End();
        rlImGuiEnd();

        DrawFPS(GetScreenWidth() - 90, 8);
        EndDrawing();
    }

    assets.unload_all();   // free GL while the context is alive (as AfterAction does)
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}
