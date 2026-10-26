#include "ProceduralModels.hpp"
#if defined(__GNUC__)
#  pragma GCC diagnostic push
#  pragma GCC diagnostic ignored "-Wmissing-field-initializers"   // raymath.h
#endif
#include <raymath.h>
#if defined(__GNUC__)
#  pragma GCC diagnostic pop
#endif
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

namespace afteraction {

namespace {

constexpr float kPi = 3.14159265358979f;

// ── MeshBuilder ───────────────────────────────────────────────────────────────
//  Accumulates coloured triangles. Curved parts (lofts, cylinders) share
//  vertices and get smooth normals; flat parts (wings, boxes) get face normals.
//  Winding is not kept consistent — models are drawn two-sided and the shader
//  turns normals toward the viewer.
class MeshBuilder {
public:
    // One fuselage cross-section: a superellipse (n = 2 ellipse, larger = boxier,
    // below 2 a lens with sharp chines) of half-width hw and half-height hh
    // centred at (x, y) at station z. Repeat a station with another colour for
    // a sharp colour edge.
    struct Section { float z, y, hw, hh, n; Color c; float x = 0.0f; };

    void loft(const std::vector<Section>& s, int segs, bool cap_end, bool cap_start = false) {
        const size_t base = verts();
        for (const Section& sec : s) {
            for (int i = 0; i < segs; ++i) {
                const float a = 2.0f * kPi * i / segs;
                const float ca = std::cos(a), sa = std::sin(a);
                const float e = 2.0f / sec.n;
                const float px = sec.x + sec.hw * std::copysign(std::pow(std::fabs(ca), e), ca);
                const float py = sec.y + sec.hh * std::copysign(std::pow(std::fabs(sa), e), sa);
                add_vertex({ px, py, sec.z }, { 0, 0, 0 }, sec.c);
            }
        }
        for (size_t r = 0; r + 1 < s.size(); ++r)
            for (int i = 0; i < segs; ++i) {
                const size_t a = base + r * segs + i, b = base + r * segs + (i + 1) % segs;
                quad_shared(a, b, b + segs, a + segs);
            }
        smooth_normals(base);
        auto cap = [&](size_t r) {
            std::vector<Vector3> ring;
            for (int i = 0; i < segs; ++i) ring.push_back(pos(base + r * segs + i));
            fan(ring, { s[r].x, s[r].y, s[r].z }, s[r].c);
        };
        if (cap_end) cap(s.size() - 1);
        if (cap_start) cap(0);
    }

    // Tapered lifting surface: leading-edge points at root and tip, chords along
    // +Z, symmetric NACA-like thickness tc (fraction of chord).
    void wing(Vector3 root_le, float root_chord, Vector3 tip_le, float tip_chord, float tc, Color c,
              Color tip_color = { 0, 0, 0, 0 }) {
        const Vector3 span = Vector3Subtract(tip_le, root_le);
        Vector3 t = Vector3CrossProduct(Vector3Normalize(span), { 0, 0, 1 });
        if (Vector3Length(t) < 1e-4f) t = { 1, 0, 0 };
        t = Vector3Normalize(t);
        static const float xs[] = { 0.0f, 0.04f, 0.12f, 0.28f, 0.5f, 0.75f, 1.0f };
        auto half = [&](float x) {
            return 5.0f * tc * (0.2969f * std::sqrt(x) - 0.126f * x - 0.3516f * x * x +
                                0.2843f * x * x * x - 0.1015f * x * x * x * x);
        };
        auto loop = [&](Vector3 le, float chord) {
            std::vector<Vector3> p;
            for (float x : xs)                                   // upper, LE → TE
                p.push_back(Vector3Add(Vector3Add(le, { 0, 0, chord * x }), Vector3Scale(t, chord * half(x))));
            for (int i = static_cast<int>(std::size(xs)) - 2; i >= 1; --i)   // lower, TE → LE
                p.push_back(Vector3Add(Vector3Add(le, { 0, 0, chord * xs[i] }), Vector3Scale(t, -chord * half(xs[i]))));
            return p;
        };
        const auto r = loop(root_le, root_chord), q = loop(tip_le, tip_chord);
        const Color tc2 = tip_color.a ? tip_color : c;
        for (size_t i = 0; i < r.size(); ++i) {
            const size_t j = (i + 1) % r.size();
            tri(r[i], r[j], q[j], c, c, tc2);
            tri(r[i], q[j], q[i], c, tc2, tc2);
        }
        fan(q, Vector3Scale(Vector3Add(q[0], q[std::size(xs) - 1]), 0.5f), tc2);
        fan(r, Vector3Scale(Vector3Add(r[0], r[std::size(xs) - 1]), 0.5f), c);
    }

    // Same surface mirrored to the left (x → −x).
    void wing_pair(Vector3 root_le, float rc, Vector3 tip_le, float tcd, float tc, Color c,
                   Color tip_color = { 0, 0, 0, 0 }) {
        wing(root_le, rc, tip_le, tcd, tc, c, tip_color);
        wing({ -root_le.x, root_le.y, root_le.z }, rc, { -tip_le.x, tip_le.y, tip_le.z }, tcd, tc, c, tip_color);
    }

    // Hexahedron from 8 corners: bottom face 0-3, top face 4-7 (same order).
    void hexa(const Vector3 p[8], Color c) {
        static const int f[6][4] = { {0,1,2,3}, {4,5,6,7}, {0,1,5,4}, {1,2,6,5}, {2,3,7,6}, {3,0,4,7} };
        for (const auto& q : f) {
            tri(p[q[0]], p[q[1]], p[q[2]], c, c, c);
            tri(p[q[0]], p[q[2]], p[q[3]], c, c, c);
        }
    }

    // Box between y0..y1 and z0..z1; half widths may differ top/bottom and front
    // (z0) vs back, and the top front edge can be set back (sloped glacis).
    // (std::optional rather than a NaN marker: NaN tests can be optimised away.)
    void block(float x, float y0, float y1, float z0, float z1, float hw_bottom, float hw_top, Color c,
               std::optional<float> top_z0 = {}, std::optional<float> top_z1 = {}) {
        const float tz0 = top_z0.value_or(z0), tz1 = top_z1.value_or(z1);
        const Vector3 p[8] = {
            { x - hw_bottom, y0, z0 }, { x + hw_bottom, y0, z0 }, { x + hw_bottom, y0, z1 }, { x - hw_bottom, y0, z1 },
            { x - hw_top,    y1, tz0 }, { x + hw_top,   y1, tz0 }, { x + hw_top,   y1, tz1 }, { x - hw_top,   y1, tz1 },
        };
        hexa(p, c);
    }

    // Cylinder / cone frustum from a (radius ra) to b (radius rb).
    void cylinder(Vector3 a, Vector3 b, float ra, float rb, int segs, Color c, bool caps = true) {
        const Vector3 ax = Vector3Normalize(Vector3Subtract(b, a));
        Vector3 u = std::fabs(ax.y) < 0.9f ? Vector3CrossProduct(ax, { 0, 1, 0 }) : Vector3CrossProduct(ax, { 1, 0, 0 });
        u = Vector3Normalize(u);
        const Vector3 v = Vector3CrossProduct(ax, u);
        const size_t base = verts();
        std::vector<Vector3> ring_a, ring_b;
        for (int i = 0; i < segs; ++i) {
            const float t = 2.0f * kPi * i / segs;
            const Vector3 d = Vector3Add(Vector3Scale(u, std::cos(t)), Vector3Scale(v, std::sin(t)));
            ring_a.push_back(Vector3Add(a, Vector3Scale(d, ra)));
            ring_b.push_back(Vector3Add(b, Vector3Scale(d, rb)));
            add_vertex(ring_a.back(), d, c);
            add_vertex(ring_b.back(), d, c);
        }
        for (int i = 0; i < segs; ++i) {
            const size_t a0 = base + 2 * i, b0 = a0 + 1;
            const size_t a1 = base + 2 * ((i + 1) % segs), b1 = a1 + 1;
            quad_shared(a0, a1, b1, b0);
        }
        if (caps) {
            if (ra > 1e-3f) fan(ring_a, a, c);
            if (rb > 1e-3f) fan(ring_b, b, c);
        }
    }

    // Rough sphere (lofted).
    void ball(Vector3 c0, float r, Color c, int segs = 12) {
        std::vector<Section> s;
        for (int i = 0; i <= 6; ++i) {
            const float a = kPi * i / 6.0f;
            s.push_back({ c0.z - r * std::cos(a), c0.y, std::max(0.01f, r * std::sin(a)),
                          std::max(0.01f, r * std::sin(a)), 2.0f, c, c0.x });
        }
        loft(s, segs, false);
    }

    // Flat-shaded triangle with its own vertices.
    void tri(Vector3 a, Vector3 b, Vector3 c, Color ca, Color cb, Color cc) {
        Vector3 n = Vector3CrossProduct(Vector3Subtract(b, a), Vector3Subtract(c, a));
        const float l = Vector3Length(n);
        if (l < 1e-9f) return;
        n = Vector3Scale(n, 1.0f / l);
        const size_t i = add_vertex(a, n, ca), j = add_vertex(b, n, cb), k = add_vertex(c, n, cc);
        index(i, j, k);
    }

    // Upload and wrap in a Model; `length` is stored by the caller.
    Model build() const {
        Mesh m{};
        m.vertexCount   = static_cast<int>(v_.size() / 3);
        m.triangleCount = static_cast<int>(idx_.size() / 3);
        m.vertices = static_cast<float*>(MemAlloc(static_cast<unsigned>(v_.size() * sizeof(float))));
        m.normals  = static_cast<float*>(MemAlloc(static_cast<unsigned>(n_.size() * sizeof(float))));
        m.colors   = static_cast<unsigned char*>(MemAlloc(static_cast<unsigned>(col_.size())));
        m.indices  = static_cast<unsigned short*>(MemAlloc(static_cast<unsigned>(idx_.size() * sizeof(unsigned short))));
        std::memcpy(m.vertices, v_.data(), v_.size() * sizeof(float));
        std::memcpy(m.normals, n_.data(), n_.size() * sizeof(float));
        std::memcpy(m.colors, col_.data(), col_.size());
        std::memcpy(m.indices, idx_.data(), idx_.size() * sizeof(unsigned short));
        UploadMesh(&m, false);
        return LoadModelFromMesh(m);
    }

private:
    size_t verts() const { return v_.size() / 3; }
    Vector3 pos(size_t i) const { return { v_[i * 3], v_[i * 3 + 1], v_[i * 3 + 2] }; }

    size_t add_vertex(Vector3 p, Vector3 n, Color c) {
        v_.insert(v_.end(), { p.x, p.y, p.z });
        n_.insert(n_.end(), { n.x, n.y, n.z });
        col_.insert(col_.end(), { c.r, c.g, c.b, c.a });
        return verts() - 1;
    }
    void index(size_t a, size_t b, size_t c) {
        idx_.insert(idx_.end(), { static_cast<unsigned short>(a), static_cast<unsigned short>(b),
                                  static_cast<unsigned short>(c) });
    }
    void quad_shared(size_t a, size_t b, size_t c, size_t d) { index(a, b, c); index(a, c, d); }

    void fan(const std::vector<Vector3>& ring, Vector3 centre, Color c) {
        for (size_t i = 0; i < ring.size(); ++i) tri(centre, ring[i], ring[(i + 1) % ring.size()], c, c, c);
    }

    // Average face normals into shared vertices added since `base`.
    void smooth_normals(size_t base) {
        for (size_t i = base * 3; i < n_.size(); ++i) n_[i] = 0.0f;
        size_t first_index = idx_.size();
        while (first_index >= 3 && idx_[first_index - 3] >= base && idx_[first_index - 2] >= base &&
               idx_[first_index - 1] >= base)
            first_index -= 3;
        for (size_t t = first_index; t < idx_.size(); t += 3) {
            const size_t a = idx_[t], b = idx_[t + 1], c = idx_[t + 2];
            const Vector3 fn = Vector3CrossProduct(Vector3Subtract(pos(b), pos(a)), Vector3Subtract(pos(c), pos(a)));
            for (size_t k : { a, b, c }) { n_[k * 3] += fn.x; n_[k * 3 + 1] += fn.y; n_[k * 3 + 2] += fn.z; }
        }
        for (size_t k = base; k < verts(); ++k) {
            Vector3 nn{ n_[k * 3], n_[k * 3 + 1], n_[k * 3 + 2] };
            const float l = Vector3Length(nn);
            nn = l > 1e-12f ? Vector3Scale(nn, 1.0f / l) : Vector3{ 0, 1, 0 };
            n_[k * 3] = nn.x; n_[k * 3 + 1] = nn.y; n_[k * 3 + 2] = nn.z;
        }
    }

    std::vector<float> v_, n_;
    std::vector<unsigned char> col_;
    std::vector<unsigned short> idx_;
};

// Rotor / propeller: `blades` thin blades of radius r in the plane normal to
// `axis` (Y = main rotor, X = tail rotor, Z = propeller), hub at the origin.
Model make_rotor(int blades, float r, float chord, Vector3 axis, Color c) {
    MeshBuilder b;
    const Vector3 ax = Vector3Normalize(axis);
    Vector3 u = std::fabs(ax.y) < 0.9f ? Vector3CrossProduct(ax, { 0, 1, 0 }) : Vector3CrossProduct(ax, { 1, 0, 0 });
    u = Vector3Normalize(u);
    const Vector3 v = Vector3CrossProduct(ax, u);
    for (int i = 0; i < blades; ++i) {
        const float a = 2.0f * kPi * i / blades;
        const Vector3 d = Vector3Add(Vector3Scale(u, std::cos(a)), Vector3Scale(v, std::sin(a)));
        const Vector3 w = Vector3CrossProduct(ax, d);              // chord direction
        const Vector3 p0 = Vector3Scale(d, r * 0.08f), p1 = Vector3Scale(d, r);
        const float t = chord * 0.08f;
        const Vector3 corners[8] = {
            Vector3Add(p0, Vector3Add(Vector3Scale(w, -chord * 0.5f), Vector3Scale(ax, -t))),
            Vector3Add(p0, Vector3Add(Vector3Scale(w,  chord * 0.5f), Vector3Scale(ax, -t))),
            Vector3Add(p1, Vector3Add(Vector3Scale(w,  chord * 0.4f), Vector3Scale(ax, -t))),
            Vector3Add(p1, Vector3Add(Vector3Scale(w, -chord * 0.4f), Vector3Scale(ax, -t))),
            Vector3Add(p0, Vector3Add(Vector3Scale(w, -chord * 0.5f), Vector3Scale(ax,  t))),
            Vector3Add(p0, Vector3Add(Vector3Scale(w,  chord * 0.5f), Vector3Scale(ax,  t))),
            Vector3Add(p1, Vector3Add(Vector3Scale(w,  chord * 0.4f), Vector3Scale(ax,  t))),
            Vector3Add(p1, Vector3Add(Vector3Scale(w, -chord * 0.4f), Vector3Scale(ax,  t))),
        };
        b.hexa(corners, c);
    }
    b.cylinder(Vector3Scale(ax, -r * 0.05f), Vector3Scale(ax, r * 0.06f), r * 0.09f, r * 0.07f, 10, c);
    return b.build();
}

// ── KAAN-style fighter ───────────────────────────────────────────────────────
ModelEntry make_fighter() {
    const Color body{ 122, 130, 140, 255 }, nose{ 98, 104, 112, 255 }, dark{ 26, 28, 32, 255 };
    const Color glass{ 156, 126, 60, 255 }, metal{ 78, 76, 74, 255 }, red{ 196, 28, 34, 255 };
    MeshBuilder b;
    // Fuselage: chined (lens-section) nose blending into a wide, flat body.
    b.loft({ { -10.6f, 0.05f, 0.03f, 0.02f, 1.6f, nose },
             {  -9.7f, 0.05f, 0.34f, 0.20f, 1.6f, nose },
             {  -8.3f, 0.08f, 0.66f, 0.40f, 1.7f, nose },
             {  -8.3f, 0.08f, 0.66f, 0.40f, 1.7f, body },
             {  -6.6f, 0.12f, 0.95f, 0.58f, 1.9f, body },
             {  -5.0f, 0.12f, 1.15f, 0.70f, 2.1f, body },
             {  -3.4f, 0.05f, 1.55f, 0.76f, 2.4f, body },
             {  -1.5f, 0.00f, 1.95f, 0.80f, 2.6f, body },
             {   1.0f, 0.00f, 2.05f, 0.80f, 2.6f, body },
             {   4.0f, 0.00f, 2.00f, 0.76f, 2.6f, body },
             {   6.5f, 0.00f, 1.85f, 0.66f, 2.6f, body },
             {   8.6f, 0.00f, 1.60f, 0.56f, 2.6f, body },
             {   9.6f, 0.00f, 1.42f, 0.50f, 2.6f, body } }, 24, true);
    // Canopy (gold-tinted, as on KAAN)
    b.loft({ { -7.7f, 0.50f, 0.05f, 0.05f, 2.0f, glass },
             { -7.0f, 0.68f, 0.36f, 0.26f, 2.0f, glass },
             { -5.9f, 0.78f, 0.46f, 0.38f, 2.0f, glass },
             { -4.6f, 0.78f, 0.44f, 0.34f, 2.0f, glass },
             { -3.7f, 0.72f, 0.30f, 0.20f, 2.0f, glass },
             { -3.7f, 0.72f, 0.30f, 0.20f, 2.0f, body },
             { -2.4f, 0.62f, 0.05f, 0.05f, 2.0f, body } }, 16, false);
    // Side intakes under the chines: dark mouth, duct fairing back into the body.
    for (float sx : { -1.0f, 1.0f })
        b.loft({ { -4.3f, -0.22f, 0.40f, 0.42f, 3.5f, dark, sx * 1.52f },
                 { -4.2f, -0.22f, 0.44f, 0.46f, 3.5f, body, sx * 1.55f },
                 { -2.0f, -0.20f, 0.46f, 0.48f, 3.5f, body, sx * 1.62f },
                 {  1.5f, -0.15f, 0.30f, 0.40f, 3.0f, body, sx * 1.45f } }, 12, false, true);
    // Wings and all-moving tailplanes
    b.wing_pair({ 1.6f, -0.05f, -2.4f }, 8.4f, { 7.0f, -0.25f, 3.0f }, 2.0f, 0.045f, body);
    b.wing_pair({ 1.5f, -0.05f, 7.1f }, 3.0f, { 4.5f, -0.05f, 9.3f }, 1.3f, 0.04f, body);
    // Canted twin fins, with a red flag band
    for (float sx : { -1.0f, 1.0f }) {
        auto at = [&](float t) {   // point t along the fin's leading edge
            return Vector3{ sx * (1.25f + 1.23f * t), 0.65f + 2.63f * t, 5.6f + 2.6f * t };
        };
        auto chord = [](float t) { return 3.8f + (1.4f - 3.8f) * t; };
        b.wing(at(0.0f), chord(0.0f), at(0.45f), chord(0.45f), 0.04f, body);
        b.wing(at(0.45f), chord(0.45f), at(0.85f), chord(0.85f), 0.04f, red);
        b.wing(at(0.85f), chord(0.85f), at(1.0f), chord(1.0f), 0.04f, body);
    }
    // Twin nozzles
    for (float sx : { -1.0f, 1.0f }) {
        b.cylinder({ sx * 0.62f, 0.0f, 9.3f }, { sx * 0.62f, 0.0f, 10.6f }, 0.58f, 0.50f, 16, metal, false);
        b.cylinder({ sx * 0.62f, 0.0f, 10.55f }, { sx * 0.62f, 0.0f, 10.6f }, 0.46f, 0.46f, 16, dark);
    }
    ModelEntry e;
    e.model = b.build();
    e.length = 21.2f;
    return e;
}

// ── T129 ATAK-style attack helicopter ────────────────────────────────────────
ModelEntry make_helicopter() {
    const Color body{ 86, 96, 80, 255 }, dark{ 34, 36, 36, 255 }, glass{ 58, 78, 92, 255 };
    MeshBuilder b;
    b.loft({ { -6.6f, 0.35f, 0.05f, 0.05f, 2.0f, body },
             { -6.1f, 0.35f, 0.32f, 0.38f, 2.2f, body },
             { -5.0f, 0.45f, 0.52f, 0.65f, 2.6f, body },
             { -3.0f, 0.65f, 0.58f, 0.95f, 2.8f, body },
             { -0.5f, 0.75f, 0.62f, 1.05f, 3.0f, body },
             {  1.5f, 0.80f, 0.58f, 0.95f, 3.0f, body },
             {  3.0f, 0.85f, 0.38f, 0.55f, 2.6f, body },
             {  5.5f, 0.95f, 0.25f, 0.30f, 2.4f, body },
             {  7.6f, 1.00f, 0.18f, 0.22f, 2.2f, body } }, 20, true);
    // Tandem cockpit: gunner in front, pilot raised behind
    b.loft({ { -5.7f, 1.00f, 0.05f, 0.05f, 2.0f, glass },
             { -5.2f, 1.12f, 0.42f, 0.26f, 2.0f, glass },
             { -4.2f, 1.25f, 0.48f, 0.36f, 2.0f, glass },
             { -3.5f, 1.25f, 0.44f, 0.30f, 2.0f, glass },
             { -3.2f, 1.25f, 0.05f, 0.05f, 2.0f, glass } }, 14, false);
    b.loft({ { -3.7f, 1.45f, 0.05f, 0.05f, 2.0f, glass },
             { -3.2f, 1.60f, 0.44f, 0.30f, 2.0f, glass },
             { -2.2f, 1.72f, 0.48f, 0.36f, 2.0f, glass },
             { -1.5f, 1.70f, 0.40f, 0.28f, 2.0f, glass },
             { -1.1f, 1.62f, 0.05f, 0.05f, 2.0f, body } }, 14, false);
    // Engine nacelles either side of the mast
    for (float s : { -1.0f, 1.0f })
        b.loft({ { -1.0f, 1.70f, 0.05f, 0.05f, 2.0f, body, s * 0.50f },
                 { -0.5f, 1.72f, 0.32f, 0.30f, 2.2f, body, s * 0.50f },
                 {  1.6f, 1.72f, 0.34f, 0.32f, 2.2f, body, s * 0.50f },
                 {  2.4f, 1.70f, 0.22f, 0.22f, 2.0f, dark, s * 0.50f } }, 12, true);
    b.cylinder({ 0, 1.75f, 0.0f }, { 0, 2.45f, 0.0f }, 0.18f, 0.14f, 10, dark);   // mast
    // Stub wings with rocket/missile pods
    b.wing_pair({ 0.55f, 0.55f, -0.7f }, 1.4f, { 2.1f, 0.48f, -0.5f }, 1.1f, 0.12f, body);
    for (float s : { -1.0f, 1.0f }) {
        b.cylinder({ s * 1.45f, 0.18f, -1.3f }, { s * 1.45f, 0.18f, 0.9f }, 0.20f, 0.20f, 10, dark);
        b.cylinder({ s * 2.00f, 0.22f, -1.1f }, { s * 2.00f, 0.22f, 0.7f }, 0.13f, 0.13f, 8, dark);
    }
    // Chin gun turret
    b.ball({ 0.0f, -0.12f, -4.9f }, 0.32f, dark, 10);
    b.cylinder({ 0.0f, -0.15f, -5.1f }, { 0.0f, -0.20f, -6.5f }, 0.05f, 0.05f, 6, dark);
    // Tail: fin, stabiliser
    b.wing({ 0.0f, 1.15f, 6.5f }, 1.3f, { 0.0f, 2.65f, 7.5f }, 0.8f, 0.10f, body);
    b.wing_pair({ 0.15f, 0.95f, 5.9f }, 0.75f, { 1.25f, 0.95f, 6.15f }, 0.5f, 0.08f, body);
    // Wheels
    for (float s : { -1.0f, 1.0f }) b.ball({ s * 0.75f, -0.45f, -2.6f }, 0.22f, dark, 8);
    b.ball({ 0.0f, 0.55f, 6.9f }, 0.12f, dark, 8);

    ModelEntry e;
    e.model = b.build();
    e.length = 14.5f;
    e.spinners.push_back({ make_rotor(4, 5.95f, 0.50f, { 0, 1, 0 }, dark), { 0.0f, 2.5f, 0.0f }, { 0, 1, 0 }, 2.2f });
    e.spinners.push_back({ make_rotor(4, 1.10f, 0.20f, { 1, 0, 0 }, dark), { -0.22f, 2.1f, 7.35f }, { 1, 0, 0 }, 7.0f });
    return e;
}

// ── ANKA-style MALE UAV ──────────────────────────────────────────────────────
ModelEntry make_uav() {
    const Color body{ 178, 184, 190, 255 }, dark{ 40, 42, 46, 255 };
    MeshBuilder b;
    b.loft({ { -4.30f, 0.06f, 0.05f, 0.05f, 2.0f, body },
             { -3.95f, 0.10f, 0.38f, 0.38f, 2.0f, body },
             { -3.00f, 0.18f, 0.55f, 0.56f, 2.2f, body },
             { -1.50f, 0.12f, 0.52f, 0.50f, 2.2f, body },
             {  0.50f, 0.06f, 0.45f, 0.45f, 2.2f, body },
             {  2.50f, 0.05f, 0.30f, 0.32f, 2.2f, body },
             {  3.90f, 0.05f, 0.14f, 0.16f, 2.0f, body },
             {  4.20f, 0.05f, 0.05f, 0.05f, 2.0f, dark } }, 18, false);
    b.wing_pair({ 0.42f, 0.25f, -1.0f }, 1.25f, { 8.65f, 0.48f, -0.75f }, 0.55f, 0.12f, body);
    b.wing_pair({ 0.12f, 0.12f, 2.55f }, 1.0f, { 1.70f, 1.25f, 3.30f }, 0.55f, 0.10f, body);   // V-tail
    b.wing({ 0.0f, -0.12f, 2.85f }, 0.7f, { 0.0f, -0.80f, 3.30f }, 0.40f, 0.10f, body);       // ventral fin
    b.ball({ 0.0f, -0.45f, -2.3f }, 0.26f, dark, 10);                                          // EO/IR ball

    ModelEntry e;
    e.model = b.build();
    e.length = 8.6f;
    e.spinners.push_back({ make_rotor(3, 1.0f, 0.18f, { 0, 0, 1 }, dark), { 0.0f, 0.05f, 4.35f }, { 0, 0, 1 }, 6.0f });
    return e;
}

// ── ANKA-3-style flying-wing UCAV ────────────────────────────────────────────
ModelEntry make_ucav() {
    const Color body{ 92, 97, 104, 255 }, dark{ 30, 32, 36, 255 }, edge{ 74, 78, 84, 255 };
    MeshBuilder b;
    // Blended centre body, flat and wide, with the intake hump on top.
    b.loft({ { -4.40f, 0.02f, 0.05f, 0.03f, 1.6f, body },
             { -3.70f, 0.05f, 0.60f, 0.22f, 1.8f, body },
             { -2.40f, 0.08f, 1.10f, 0.40f, 2.0f, body },
             { -0.60f, 0.10f, 1.30f, 0.50f, 2.2f, body },
             {  1.40f, 0.08f, 1.15f, 0.42f, 2.2f, body },
             {  3.00f, 0.05f, 0.80f, 0.28f, 2.4f, body },
             {  3.70f, 0.03f, 0.55f, 0.18f, 2.6f, body } }, 20, true);
    // Dorsal intake: a flattened scoop with a dark mouth.
    b.loft({ { -1.90f, 0.50f, 0.42f, 0.14f, 3.0f, dark },
             { -1.80f, 0.52f, 0.46f, 0.16f, 3.0f, body },
             {  0.20f, 0.52f, 0.40f, 0.14f, 3.0f, body },
             {  1.60f, 0.42f, 0.10f, 0.05f, 2.0f, body } }, 12, false, true);
    // Exhaust slot at the tail.
    b.block(0, -0.06f, 0.14f, 3.55f, 3.75f, 0.45f, 0.42f, dark);
    // Cranked lambda wing: inner panel, then the outer panel with a slight
    // forward kink in the trailing edge (the ANKA-3 "W" planform).
    b.wing_pair({ 0.4f, 0.0f, -3.9f }, 7.2f, { 3.2f, 0.0f, -1.2f }, 3.2f, 0.07f, body);
    b.wing_pair({ 3.2f, 0.0f, -1.2f }, 3.2f, { 6.0f, 0.0f, 1.3f }, 0.9f, 0.06f, body, edge);
    ModelEntry e;
    e.model = b.build();
    e.length = 12.0f;   // span
    return e;
}

// ── HÜRKUŞ-style turboprop trainer ───────────────────────────────────────────
ModelEntry make_prop_trainer() {
    const Color body{ 228, 230, 232, 255 }, red{ 200, 32, 38, 255 }, dark{ 34, 36, 40, 255 };
    const Color glass{ 70, 92, 112, 255 }, metal{ 96, 98, 102, 255 };
    MeshBuilder b;
    // Fuselage: spinner, long turboprop cowling, tandem cockpit, slim tail cone.
    b.loft({ { -5.75f, 0.10f, 0.03f, 0.03f, 2.0f, red },
             { -5.55f, 0.10f, 0.22f, 0.22f, 2.0f, red },
             { -5.30f, 0.10f, 0.30f, 0.30f, 2.0f, red },
             { -5.30f, 0.10f, 0.30f, 0.30f, 2.0f, body },
             { -5.10f, 0.08f, 0.46f, 0.50f, 2.4f, body },
             { -3.80f, 0.05f, 0.56f, 0.64f, 2.6f, body },
             { -2.40f, 0.05f, 0.60f, 0.70f, 2.6f, body },
             {  0.50f, 0.08f, 0.58f, 0.68f, 2.6f, body },
             {  2.50f, 0.16f, 0.38f, 0.48f, 2.4f, body },
             {  4.60f, 0.30f, 0.18f, 0.26f, 2.2f, body },
             {  5.50f, 0.36f, 0.08f, 0.14f, 2.0f, body } }, 20, true);
    // Long tandem canopy (student front, instructor raised behind).
    b.loft({ { -3.00f, 0.62f, 0.05f, 0.05f, 2.0f, glass },
             { -2.40f, 0.78f, 0.40f, 0.30f, 2.0f, glass },
             { -1.20f, 0.88f, 0.46f, 0.40f, 2.0f, glass },
             {  0.20f, 0.92f, 0.44f, 0.40f, 2.0f, glass },
             {  1.10f, 0.82f, 0.30f, 0.24f, 2.0f, glass },
             {  1.10f, 0.82f, 0.30f, 0.24f, 2.0f, body },
             {  2.20f, 0.60f, 0.05f, 0.05f, 2.0f, body } }, 16, false);
    // Exhaust stubs either side of the cowling.
    for (float sx : { -1.0f, 1.0f })
        b.cylinder({ sx * 0.48f, 0.05f, -4.0f }, { sx * 0.70f, 0.02f, -3.7f }, 0.13f, 0.11f, 10, metal);
    // Surfaces with solid red tips (trainer scheme): the outer `tip` fraction
    // of each panel is a separate red piece, so the colour doesn't fade.
    auto two_tone = [&](Vector3 root, float rc, Vector3 tip_le, float tc, float th, float tip, bool pair) {
        const float t = 1.0f - tip;
        const Vector3 mid = Vector3Lerp(root, tip_le, t);
        const float mc = rc + (tc - rc) * t;
        if (pair) { b.wing_pair(root, rc, mid, mc, th, body); b.wing_pair(mid, mc, tip_le, tc, th, red); }
        else      { b.wing(root, rc, mid, mc, th, body);      b.wing(mid, mc, tip_le, tc, th, red); }
    };
    // Low wing with slight dihedral; tailplane and fin.
    two_tone({ 0.5f, -0.40f, -1.10f }, 2.30f, { 5.0f, -0.08f, -0.55f }, 1.15f, 0.13f, 0.22f, true);
    two_tone({ 0.2f, 0.32f, 3.85f }, 1.30f, { 2.0f, 0.36f, 4.45f }, 0.75f, 0.09f, 0.30f, true);
    two_tone({ 0.0f, 0.40f, 3.45f }, 1.90f, { 0.0f, 2.10f, 4.85f }, 0.90f, 0.09f, 0.30f, false);
    // Ventral strake / antenna
    b.wing({ 0.0f, -0.55f, 3.6f }, 0.9f, { 0.0f, -0.85f, 4.2f }, 0.4f, 0.08f, body);

    ModelEntry e;
    e.model = b.build();
    e.length = 11.2f;
    e.spinners.push_back({ make_rotor(4, 1.30f, 0.24f, { 0, 0, 1 }, dark), { 0.0f, 0.10f, -5.62f }, { 0, 0, 1 }, 7.0f });
    return e;
}

// ── C-130-style tactical transport ───────────────────────────────────────────
ModelEntry make_transport() {
    const Color body{ 132, 138, 146, 255 }, dark{ 34, 36, 40, 255 }, glass{ 52, 66, 80, 255 };
    const Color nacelle{ 120, 126, 134, 255 }, radome{ 96, 100, 106, 255 };
    MeshBuilder b;
    // Fuselage: blunt radome nose, constant box-round cabin, upswept tail over the ramp.
    b.loft({ { -14.90f, -0.20f, 0.30f, 0.30f, 2.0f, radome },
             { -14.40f, -0.15f, 1.20f, 1.30f, 2.0f, radome },
             { -13.90f, -0.05f, 1.65f, 1.80f, 2.2f, radome },
             { -13.90f, -0.05f, 1.65f, 1.80f, 2.2f, body },
             { -12.40f,  0.05f, 2.05f, 2.20f, 2.4f, body },
             { -10.00f,  0.10f, 2.20f, 2.35f, 2.6f, body },
             {   5.50f,  0.10f, 2.20f, 2.35f, 2.6f, body },
             {   7.80f,  0.60f, 2.00f, 1.90f, 2.4f, body },
             {  10.50f,  1.35f, 1.50f, 1.20f, 2.2f, body },
             {  13.20f,  1.95f, 0.80f, 0.60f, 2.0f, body },
             {  14.80f,  2.25f, 0.20f, 0.22f, 2.0f, body } }, 24, true);
    // Flight-deck glazing: a shallow dark band over the nose.
    b.loft({ { -14.25f, 0.90f, 0.20f, 0.10f, 2.0f, glass },
             { -13.85f, 1.25f, 1.45f, 0.72f, 2.4f, glass },
             { -12.90f, 1.62f, 1.75f, 0.80f, 2.4f, glass },
             { -12.10f, 1.75f, 1.70f, 0.72f, 2.4f, glass },
             { -12.10f, 1.75f, 1.70f, 0.72f, 2.4f, body },
             { -10.80f, 1.85f, 1.10f, 0.50f, 2.2f, body },
             {  -9.80f, 1.90f, 0.10f, 0.05f, 2.0f, body } }, 18, false);
    // Main-gear sponsons along the lower sides.
    for (float sx : { -1.0f, 1.0f })
        b.loft({ { -3.60f, -1.45f, 0.10f, 0.10f, 2.0f, body, sx * 2.05f },
                 { -2.60f, -1.45f, 0.55f, 0.80f, 2.6f, body, sx * 2.05f },
                 {  2.80f, -1.45f, 0.55f, 0.80f, 2.6f, body, sx * 2.05f },
                 {  3.80f, -1.45f, 0.10f, 0.10f, 2.0f, body, sx * 2.05f } }, 14, false);
    // High wing with slight dihedral.
    b.wing_pair({ 2.0f, 2.25f, -2.6f }, 4.9f, { 20.2f, 2.65f, -1.3f }, 2.5f, 0.15f, body);
    // Four turboprop nacelles hanging forward of the wing, with spinners.
    const float nx[2] = { 5.1f, 10.6f };
    for (float sx : { -1.0f, 1.0f })
        for (float x : nx) {
            const float cx = sx * x, cy = 2.05f;
            b.loft({ { -7.05f, cy, 0.10f, 0.10f, 2.0f, dark, cx },
                     { -6.90f, cy, 0.30f, 0.30f, 2.0f, dark, cx },
                     { -6.60f, cy, 0.48f, 0.48f, 2.0f, nacelle, cx },
                     { -5.80f, cy - 0.05f, 0.62f, 0.78f, 2.4f, nacelle, cx },
                     { -1.00f, cy - 0.05f, 0.62f, 0.80f, 2.4f, nacelle, cx },
                     {  1.80f, cy + 0.05f, 0.32f, 0.40f, 2.2f, nacelle, cx } }, 14, true);
        }
    // Tail: big fin and tailplane set on the upswept rear fuselage.
    b.wing({ 0.0f, 2.30f, 7.6f }, 6.6f, { 0.0f, 9.60f, 11.9f }, 3.0f, 0.12f, body);
    b.wing_pair({ 0.5f, 2.45f, 10.2f }, 4.3f, { 8.0f, 2.55f, 12.3f }, 2.1f, 0.11f, body);

    ModelEntry e;
    e.model = b.build();
    e.length = 40.4f;   // span (larger than the 29.8 m length)
    for (float sx : { -1.0f, 1.0f })
        for (float x : nx)
            e.spinners.push_back({ make_rotor(4, 2.05f, 0.42f, { 0, 0, 1 }, dark),
                                   { sx * x, 2.05f, -7.0f }, { 0, 0, 1 }, sx > 0 ? 5.0f : 5.3f });
    return e;
}

// ── Air-to-air missile ───────────────────────────────────────────────────────
ModelEntry make_missile() {
    const Color white{ 226, 228, 230, 255 }, nose{ 64, 66, 70, 255 }, band{ 170, 140, 40, 255 };
    MeshBuilder b;
    b.loft({ { -1.83f, 0.0f, 0.005f, 0.005f, 2.0f, nose },
             { -1.55f, 0.0f, 0.060f, 0.060f, 2.0f, nose },
             { -1.30f, 0.0f, 0.085f, 0.085f, 2.0f, nose },
             { -1.18f, 0.0f, 0.090f, 0.090f, 2.0f, nose },
             { -1.18f, 0.0f, 0.090f, 0.090f, 2.0f, white },
             { -0.62f, 0.0f, 0.090f, 0.090f, 2.0f, white },
             { -0.62f, 0.0f, 0.090f, 0.090f, 2.0f, band },
             { -0.50f, 0.0f, 0.090f, 0.090f, 2.0f, band },
             { -0.50f, 0.0f, 0.090f, 0.090f, 2.0f, white },
             {  1.82f, 0.0f, 0.090f, 0.090f, 2.0f, white } }, 12, true);
    for (int i = 0; i < 4; ++i) {
        const float a = kPi * 0.25f + i * kPi * 0.5f, cx = std::cos(a), cy = std::sin(a);
        b.wing({ cx * 0.08f, cy * 0.08f, 1.30f }, 0.45f, { cx * 0.32f, cy * 0.32f, 1.45f }, 0.30f, 0.05f, white);
        b.wing({ cx * 0.08f, cy * 0.08f, -0.35f }, 1.0f, { cx * 0.17f, cy * 0.17f, -0.25f }, 0.8f, 0.05f, white);
    }
    ModelEntry e;
    e.model = b.build();
    e.length = 3.65f;
    return e;
}

// A row of wheel pairs (wheeled vehicles).
void wheels(MeshBuilder& b, std::initializer_list<float> zs, float x, float r, Color c) {
    for (float z : zs)
        for (float s : { -1.0f, 1.0f })
            b.cylinder({ s * (x - 0.18f), r, z }, { s * (x + 0.18f), r, z }, r, r, 12, c);
}

// ── KORKUT-style air-defence vehicle (tracked ACV-30 chassis) ────────────────
ModelEntry make_air_defence() {
    const Color body{ 96, 104, 74, 255 }, dark{ 30, 31, 30, 255 }, radar{ 120, 126, 110, 255 };
    MeshBuilder b;
    for (float sx : { -1.0f, 1.0f }) {
        // Track run: longer on top than on the ground, so the ends slope.
        b.block(sx * 1.18f, 0.0f, 0.85f, -2.9f, 2.9f, 0.27f, 0.27f, dark, -3.45f, 3.25f);
        for (int i = 0; i < 6; ++i) {   // road wheels
            const float z = -2.45f + i * 0.98f;
            b.cylinder({ sx * 1.46f, 0.36f, z }, { sx * 1.47f, 0.36f, z }, 0.33f, 0.33f, 12, { 58, 60, 52, 255 });
        }
    }
    b.block(0, 0.55f, 1.80f, -3.6f, 3.4f, 1.48f, 1.40f, body, -2.6f);                    // hull, sloped glacis
    b.block(0, 1.80f, 2.65f, -1.0f, 1.8f, 1.05f, 0.85f, body, -0.55f, 1.6f);              // turret
    for (float sx : { -1.0f, 1.0f }) {
        b.block(sx * 1.12f, 2.00f, 2.50f, -1.3f, 0.6f, 0.17f, 0.15f, body);              // gun housings
        b.cylinder({ sx * 1.12f, 2.25f, -1.3f }, { sx * 1.12f, 2.25f, -4.1f }, 0.07f, 0.06f, 8, dark);
    }
    b.cylinder({ 0, 2.65f, 1.1f }, { 0, 3.05f, 1.1f }, 0.10f, 0.10f, 8, dark);           // radar post
    b.block(0, 3.05f, 3.75f, 1.0f, 1.2f, 0.75f, 0.70f, radar);                           // search radar
    ModelEntry e;
    e.model = b.build();
    e.length = 7.6f;
    return e;
}

// ── 6×6 armoured vehicle ─────────────────────────────────────────────────────
ModelEntry make_armour() {
    const Color body{ 84, 92, 68, 255 }, dark{ 30, 31, 30, 255 }, glass{ 60, 76, 86, 255 };
    MeshBuilder b;
    b.block(0, 0.60f, 2.05f, -3.4f, 3.4f, 1.25f, 1.15f, body, -2.4f);
    b.block(0, 1.60f, 1.95f, -2.75f, -2.55f, 0.95f, 0.90f, glass, -2.45f, -2.45f);   // vision blocks
    wheels(b, { -2.3f, 0.0f, 2.3f }, 1.28f, 0.58f, dark);
    b.block(0, 2.05f, 2.55f, -0.7f, 0.8f, 0.60f, 0.45f, body);                       // turret
    b.cylinder({ 0, 2.35f, -0.6f }, { 0, 2.35f, -2.6f }, 0.05f, 0.05f, 6, dark);
    ModelEntry e;
    e.model = b.build();
    e.length = 6.8f;
    return e;
}

// ── HİSAR-O-style SAM launcher (6×6 truck, raised missile canisters) ─────────
ModelEntry make_sam() {
    const Color body{ 90, 98, 72, 255 }, dark{ 30, 31, 30, 255 }, glass{ 60, 76, 86, 255 };
    const Color pod{ 104, 112, 84, 255 };
    MeshBuilder b;
    b.block(0, 0.85f, 1.15f, -4.6f, 4.8f, 1.15f, 1.15f, dark);                      // chassis rails
    b.block(0, 1.15f, 3.05f, -4.9f, -2.9f, 1.25f, 1.15f, body, -4.4f);              // cab
    b.block(0, 2.30f, 2.85f, -4.64f, -4.50f, 1.05f, 1.0f, glass, -4.49f, -4.36f);   // windscreen (on the slope)
    b.block(0, 1.15f, 1.65f, -2.7f, 4.8f, 1.25f, 1.25f, body);                      // flat bed
    wheels(b, { -3.6f, 1.6f, 3.2f }, 1.20f, 0.60f, dark);

    // Launcher: a box of 2×3 canisters raised to 50°, pointing forward/up from
    // a turntable at the rear of the bed.
    const float elev = 50.0f * kPi / 180.0f;
    const Vector3 ax{ 0.0f, std::sin(elev), -std::cos(elev) };       // along the canisters
    const Vector3 up{ 0.0f, std::cos(elev),  std::sin(elev) };
    const Vector3 side{ 1.0f, 0.0f, 0.0f };
    const Vector3 pivot{ 0.0f, 2.05f, 3.9f };
    const float len = 5.2f, half_w = 0.95f, half_h = 0.62f;
    const Vector3 base_c = Vector3Add(pivot, Vector3Scale(up, half_h));
    auto corner = [&](float a, float sx, float sy) {
        return Vector3Add(base_c, Vector3Add(Vector3Scale(ax, a * len),
                          Vector3Add(Vector3Scale(side, sx * half_w), Vector3Scale(up, sy * half_h))));
    };
    const Vector3 box[8] = { corner(0, -1, -1), corner(0, 1, -1), corner(1, 1, -1), corner(1, -1, -1),
                             corner(0, -1,  1), corner(0, 1,  1), corner(1, 1,  1), corner(1, -1,  1) };
    b.hexa(box, pod);
    for (int r = 0; r < 2; ++r)                                        // canister mouths
        for (int c = 0; c < 3; ++c) {
            const Vector3 m = Vector3Add(Vector3Add(base_c, Vector3Scale(ax, len + 0.02f)),
                                         Vector3Add(Vector3Scale(side, (c - 1) * 0.62f),
                                                    Vector3Scale(up, (r - 0.5f) * 0.6f)));
            b.cylinder(Vector3Subtract(m, Vector3Scale(ax, 0.05f)), m, 0.24f, 0.24f, 12, dark);
        }
    b.cylinder({ 0, 1.65f, 3.9f }, { 0, 2.05f, 3.9f }, 0.75f, 0.75f, 14, body);      // turntable
    b.block(0, 1.65f, 2.35f, 0.2f, 1.6f, 1.1f, 1.0f, body);                          // equipment box
    b.cylinder({ 0, 2.35f, 0.9f }, { 0, 3.3f, 0.9f }, 0.08f, 0.08f, 8, dark);        // EO / radar mast
    b.block(0, 3.3f, 3.75f, 0.75f, 1.05f, 0.45f, 0.42f, pod);
    ModelEntry e;
    e.model = b.build();
    e.length = 9.8f;
    return e;
}

// ── MİLGEM (Ada-class) corvette ──────────────────────────────────────────────
ModelEntry make_corvette() {
    const Color hull{ 128, 134, 140, 255 }, sup{ 150, 155, 160, 255 };
    const Color dark{ 40, 42, 46, 255 }, red{ 120, 40, 36, 255 };
    MeshBuilder b;
    // Hull: waterline at y = 0, a little boot-topping below, deck at +5.
    b.loft({ { -49.8f, 3.0f, 0.10f, 2.5f, 4.0f, hull },
             { -44.0f, 2.6f, 2.60f, 3.4f, 4.0f, hull },
             { -35.0f, 2.3f, 5.00f, 3.7f, 4.5f, hull },
             { -20.0f, 2.2f, 7.00f, 3.8f, 5.0f, hull },
             {   0.0f, 2.2f, 7.20f, 3.8f, 5.0f, hull },
             {  30.0f, 2.2f, 6.60f, 3.8f, 5.0f, hull },
             {  49.0f, 2.4f, 5.60f, 3.6f, 5.0f, hull } }, 24, true);
    b.loft({ { -49.0f, -1.1f, 0.10f, 0.4f, 4.0f, red },        // red anti-fouling at the waterline
             { -40.0f, -1.1f, 3.80f, 0.4f, 4.0f, red },
             {   0.0f, -1.1f, 7.00f, 0.4f, 5.0f, red },
             {  48.0f, -1.1f, 5.50f, 0.4f, 5.0f, red } }, 24, true);
    b.block(0, 5.9f, 10.2f, -15.0f, 9.0f, 5.2f, 4.6f, sup, -13.0f, 8.0f);              // superstructure
    b.block(0, 10.2f, 12.8f, -13.0f, -3.0f, 4.0f, 3.6f, sup, -11.5f, -3.5f);           // bridge
    b.block(0, 12.8f, 21.0f, -7.0f, -2.5f, 1.6f, 0.4f, sup, -5.5f, -3.5f);             // mast
    b.block(0, 10.2f, 13.0f, 3.0f, 8.0f, 1.8f, 1.5f, sup, 3.5f, 7.5f);                 // funnel
    b.block(0, 5.9f, 10.0f, 12.0f, 24.0f, 5.6f, 5.4f, sup);                            // hangar
    b.block(0, 5.9f, 7.4f, -31.0f, -27.0f, 1.6f, 1.2f, sup, -30.0f, -27.5f);          // 76 mm gun
    b.cylinder({ 0, 6.9f, -30.0f }, { 0, 7.2f, -36.0f }, 0.13f, 0.11f, 8, dark);
    for (float s : { -1.0f, 1.0f })
        b.block(s * 2.2f, 5.9f, 7.6f, 9.5f, 11.5f, 0.9f, 0.9f, sup);                   // missile canisters
    ModelEntry e;
    e.model = b.build();
    e.length = 99.5f;
    return e;
}

// ── Unknown: diamond marker ──────────────────────────────────────────────────
ModelEntry make_marker() {
    MeshBuilder b;
    const Color c{ 190, 205, 220, 255 };
    const Vector3 top{ 0, 1.6f, 0 }, bottom{ 0, -1.6f, 0 };
    const Vector3 ring[4] = { { 0, 0, -3.5f }, { 2.0f, 0, 0 }, { 0, 0, 2.5f }, { -2.0f, 0, 0 } };
    for (int i = 0; i < 4; ++i) {
        b.tri(top, ring[i], ring[(i + 1) % 4], c, c, c);
        b.tri(bottom, ring[(i + 1) % 4], ring[i], c, c, c);
    }
    ModelEntry e;
    e.model = b.build();
    e.length = 6.0f;
    return e;
}

} // namespace

void build_procedural_models(std::unordered_map<uint16_t, ModelEntry>& out, ModelEntry& fallback) {
    out[net::TYPE_JET]     = make_fighter();
    out[net::TYPE_HELO]    = make_helicopter();
    out[net::TYPE_UAV]     = make_uav();
    out[net::TYPE_UCAV]    = make_ucav();
    out[net::TYPE_SAM]     = make_sam();
    out[net::TYPE_PROP]    = make_prop_trainer();
    out[net::TYPE_TRANSPORT] = make_transport();
    out[net::TYPE_MISSILE] = make_missile();
    out[net::TYPE_AAA]     = make_air_defence();
    out[net::TYPE_GROUND]  = make_armour();
    out[net::TYPE_SHIP]    = make_corvette();
    fallback = make_marker();
}

} // namespace afteraction
