// tdem_c.cpp — C wrapper over tdem::Database.
#include "tdem/tdem_c.h"
#include "tdem/tdem.hpp"

struct tdem_db {
    tdem::Database db;
};

extern "C" {

tdem_db* tdem_open_dir(const char* dir) {
    auto* h = new tdem_db;
    if (!dir || h->db.add_directory(dir) == 0) { delete h; return nullptr; }
    return h;
}

tdem_db* tdem_open_file(const char* path) {
    auto* h = new tdem_db;
    if (!path || !h->db.add_pack(path)) { delete h; return nullptr; }
    return h;
}

int tdem_add_pack(tdem_db* db, const char* path) {
    return db && path && db->db.add_pack(path) ? 1 : 0;
}

void tdem_close(tdem_db* db) { delete db; }

int tdem_height(const tdem_db* db, double lat, double lon, float* out) {
    if (!db) return 0;
    auto h = db->db.height(lat, lon);
    if (!h) return 0;
    if (out) *out = *h;
    return 1;
}

int tdem_height_lod(const tdem_db* db, double lat, double lon, float min_resolution_m, float* out) {
    if (!db) return 0;
    auto h = db->db.height(lat, lon, min_resolution_m);
    if (!h) return 0;
    if (out) *out = *h;
    return 1;
}

long tdem_sample_grid(const tdem_db* db, double north, double west, double dlat, double dlon,
                      int rows, int cols, float* out, float fallback) {
    if (!db || !out || rows <= 0 || cols <= 0) return 0;
    return static_cast<long>(db->db.sample_grid(north, west, dlat, dlon, rows, cols, out, fallback));
}

float tdem_resolution_at(const tdem_db* db, double lat, double lon) {
    return db ? db->db.resolution_at(lat, lon) : 0.0f;
}

int tdem_layer_count(const tdem_db* db) {
    return db ? static_cast<int>(db->db.layers().size()) : 0;
}

const char* tdem_layer_name(const tdem_db* db, int index) {
    if (!db || index < 0 || index >= tdem_layer_count(db)) return nullptr;
    return db->db.layers()[static_cast<size_t>(index)].name.c_str();
}

} // extern "C"
