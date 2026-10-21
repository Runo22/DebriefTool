/* tdem_c.h — C interface to the TDEM reader, for use from C, C#, Python
 * (ctypes), etc. Build the library as shared (-DTDEM_SHARED=ON) to get a DLL/.so.
 *
 *   tdem_db* db = tdem_open_dir("terrain/data");
 *   float h;
 *   if (tdem_height(db, 40.0789, 32.5656, &h)) printf("%.1f m\n", h);
 *   tdem_close(db);
 */
#ifndef TDEM_C_H
#define TDEM_C_H

#include "export.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tdem_db tdem_db;

/* Open every *.tdem in a directory / a single file. NULL on failure. */
TDEM_API tdem_db* tdem_open_dir(const char* dir);
TDEM_API tdem_db* tdem_open_file(const char* path);
/* Add another pack to an open database. Returns 1 on success. */
TDEM_API int      tdem_add_pack(tdem_db* db, const char* path);
TDEM_API void     tdem_close(tdem_db* db);

/* Height in metres MSL. Returns 1 and writes *out if covered, else 0. */
TDEM_API int   tdem_height(const tdem_db* db, double lat, double lon, float* out);
TDEM_API int   tdem_height_lod(const tdem_db* db, double lat, double lon,
                               float min_resolution_m, float* out);
/* Row-major grid, row 0 = north. Returns the number of covered samples. */
TDEM_API long  tdem_sample_grid(const tdem_db* db, double north, double west,
                                double dlat, double dlon, int rows, int cols,
                                float* out, float fallback);
TDEM_API float tdem_resolution_at(const tdem_db* db, double lat, double lon);
TDEM_API int   tdem_layer_count(const tdem_db* db);
TDEM_API const char* tdem_layer_name(const tdem_db* db, int index);

#ifdef __cplusplus
}
#endif
#endif /* TDEM_C_H */
