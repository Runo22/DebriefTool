# Roadmap — Runtime / Config-Driven Model Loading

Goal: bind 3D models to entity types **without recompiling** — via a config file
and/or the Settings UI — and load them **at startup and at runtime** while keeping
the window opening **instantly** (no multi-second stall).

---

## Where we are today

| Aspect | Current behaviour | File |
|---|---|---|
| Default models | Only **procedural** shapes are built at startup; no file models | `Application::init_assets()` (`src/app/Application.cpp`) |
| File loading | `AssetManager::load()` → `convert_assimp()`: Assimp `ReadFile` **and** `UploadMesh` run **synchronously on the caller** | `src/render/AssetManager.cpp` |
| Trigger | `on_load_model(type, path)` callback exists but **no UI/config calls it** | `src/app/Application.cpp`, `AfterActionUI` |
| Binding | `get_for_type(type)`: loaded → procedural → fallback cube | `AssetManager` |
| Entity → model | `RenderModel.model_ptr` is **cached at spawn** in `get_or_create_entity()` | `src/app/Application.cpp` |

Because nothing loads files yet, **startup is already fast**. The work below adds
file models *without* reintroducing a startup stall.

### Two hard constraints
1. **GL is single-threaded.** `UploadMesh` / `LoadModelFromMesh` must run on the
   main (render) thread. Only the Assimp **parse + CPU mesh build** is safe on a
   worker thread. So async loading = *parse off-thread, upload on-thread*.
2. **Cached `model_ptr`.** Entities created before a model finishes loading keep
   their old pointer. Loading must be able to **hot-swap** existing entities.
   (Good news: pointers into the `named_`/`procedural_` maps are node-stable, so
   they don't dangle on rehash — we only need to *repoint*, not worry about
   invalidation.)

---

## Phase 0 — Config-driven mappings (no recompile)  ·  small effort

Add a model manifest so type→model bindings live in a file, not in code.

- **Schema** — `assets/models.yaml` (or a `models:` section in
  `afteraction_config.yaml`):
  ```yaml
  models:
    - type: 1            # EntityTypeId (1=jet, 2=missile, …)
      file: models/f16.glb
      scale: 0.01        # 0.01 if the model is authored in centimetres
      tint:  [255,255,255]
      base_rot: [0, 90, 0]   # yaw,pitch,roll degrees — fix model facing
  ```
- **Loader** — parse the manifest in `init_assets()`, call `assets_.load()` +
  `assets_.map_type()` per entry. Extend `ModelEntry`/`load()` to accept
  tint + base_rot (currently only `import_scale`).
- **Result**: drop a file in `assets/models/`, edit the YAML, **restart** → new
  models. No compiler needed.
- ⚠️ Still synchronous — keep it non-blocking by deferring to Phase 2, or by only
  loading a few small models. (Phase 2 removes this caveat entirely.)

**Deliverable:** models configurable via a text file.

---

## Phase 1 — Settings UI for mappings  ·  medium effort

A **Settings → Models** tab so users bind models live, no file editing.

- Table of rows: `type` (combo) · `file` (path input + browse) · `scale` ·
  `tint` · `base_rot` · **Load / Reload / Clear**.
- Wire the existing `on_load_model` callback; add `on_clear_model`,
  `on_save_models` (writes the Phase 0 manifest so choices persist).
- **Hot-swap** existing entities (see “Hot-swap design” below) so a load takes
  effect immediately, not just for newly-spawned tracks.
- Surface load **errors** inline (Assimp message) instead of only the log.

**Deliverable:** add/replace models at runtime from the UI, persisted to config.

---

## Phase 2 — Async load = guaranteed fast startup  ·  larger effort

Split the heavy work so the window opens immediately and models stream in.

- **Refactor** `convert_assimp()` into:
  - `parse_to_cpu(path, scale) -> MeshData` (Assimp + CPU arrays) — **worker thread**.
  - `upload(MeshData) -> ModelEntry` (`UploadMesh`/`LoadModelFromMesh`) — **main thread**.
- **Job queue** in `AssetManager`:
  - `request_load(name, path, …)` enqueues a job; a worker thread (or pool)
    parses; finished CPU meshes go on a main-thread "ready" queue.
  - `pump_uploads(budget)` called once per frame from `tick()` uploads up to
    *budget* ready meshes (e.g. 1–2/frame) → registers → repoints entities.
- **Startup flow:** `InitWindow → first frame renders procedural shapes
  instantly`; manifest loads are *requested*, not awaited. Models pop in over the
  next frames. Optional small "loading models… N left" chip.
- Per-frame upload budget prevents a single big model from hitching the frame.

**Deliverable:** any number/size of models, **zero** added startup latency.

---

## Phase 3 — Polish  ·  optional

- **Hot-reload** on file mtime change (auto re-import while iterating on a model).
- **Base-rotation tuner**: live yaw/pitch/roll sliders to fix model facing, saved
  to the manifest.
- Material/texture import (currently mesh-only, flat tint).
- Model cache / dedupe; validation + friendly errors; per-source overrides.

---

## Hot-swap design (shared by Phases 1–2)

`RenderModel.model_ptr` is cached at spawn, so newly-loaded models don't reach
existing entities. Two clean options:

- **A. Resolve-per-frame (simplest):** drop the cached pointer; store `type_id`
  in `RenderModel` and call `get_for_type(meta.type)` in the render loop. A few
  `unordered_map` lookups/frame — negligible, and always current. *Recommended.*
- **B. Repoint-on-ready:** keep the cache; when a model finishes loading, query
  the ECS for entities of that type and update their `RenderModel` (tint/scale/
  base_rot/model_ptr). More code, marginally faster draw.

Either makes runtime/async loading correct; A is the least error-prone.

---

## Suggested order

1. **Phase 0** (manifest + extended `load()`): unblocks "no recompile" today.
2. **Phase 2 split** (async): the real fix for fast startup — do before shipping
   any large models.
3. **Phase 1** (Settings UI on top of the async queue + hot-swap).
4. **Phase 3** as needed.

> Fast-open guarantee: at no phase does Assimp work run before the first frame.
> Phase 0 defers manifest loads to the async queue (Phase 2); until that lands,
> keep manifest models small or load them a few frames after the window appears.
