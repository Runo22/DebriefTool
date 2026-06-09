#include "Recorder.hpp"
#include <cstring>
#include <chrono>
#include <thread>

namespace debrief::persist {

Recorder::Recorder()  = default;
Recorder::~Recorder() { stop(); }

bool Recorder::start(const std::filesystem::path& path,
                     const std::string& session_name,
                     const SceneOrigin& origin) noexcept
{
    if (recording_.load()) return false;
    path_ = path;

    file_.open(path, std::ios::binary | std::ios::trunc);
    if (!file_) return false;

    header_ = {};
    header_.magic   = kFileMagic;
    header_.version = kFileVersion;
    auto now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    header_.start_time_ns = now_ns;

    auto name_len = std::min(session_name.size(), size_t(31));
    std::memcpy(header_.session_name, session_name.c_str(), name_len);

    // Write placeholder header (will be overwritten in finalise()).
    file_.write(reinterpret_cast<const char*>(&header_), sizeof(header_));
    file_.write(reinterpret_cast<const char*>(&origin),  sizeof(origin));

    frame_index_.clear();
    frames_written_.store(0);
    stop_requested_.store(false);
    recording_.store(true);

    io_thread_ = std::thread(&Recorder::io_thread_fn, this);
    return true;
}

void Recorder::submit(uint64_t ts_ns, uint16_t source_id,
                      const std::vector<net::EntityState>& entities) noexcept
{
    if (!recording_.load()) return;
    std::lock_guard lock(queue_mu_);
    write_queue_.push_back({ts_ns, source_id, entities});
}

void Recorder::stop() noexcept {
    if (!recording_.load()) return;
    stop_requested_.store(true);
    if (io_thread_.joinable()) io_thread_.join();
}

void Recorder::io_thread_fn() {
    while (!stop_requested_.load() || [&]{
            std::lock_guard l(queue_mu_);
            return !write_queue_.empty();
        }())
    {
        // Swap queues to minimise lock hold time.
        {
            std::lock_guard lock(queue_mu_);
            std::swap(write_queue_, swap_queue_);
        }
        for (auto& cmd : swap_queue_)
            write_frame(cmd);
        swap_queue_.clear();

        if (!stop_requested_.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    finalise();
    recording_.store(false);
}

void Recorder::write_frame(const WriteCmd& cmd) {
    if (!file_) return;

    const auto fc = frames_written_.load();

    // Store an index entry every kIndexInterval frames.
    if (fc % kIndexInterval == 0) {
        IndexEntry ie{};
        ie.timestamp_ns = cmd.timestamp_ns;
        ie.file_offset  = static_cast<uint64_t>(file_.tellp());
        frame_index_.push_back(ie);
    }

    FrameHeader fh{};
    fh.timestamp_ns = cmd.timestamp_ns;
    fh.source_id    = cmd.source_id;
    fh.entity_count = static_cast<uint16_t>(
        std::min(cmd.entities.size(), size_t(65535)));
    fh.frame_bytes  = sizeof(FrameHeader) + fh.entity_count * sizeof(EntitySnapshot);

    file_.write(reinterpret_cast<const char*>(&fh), sizeof(fh));

    for (uint16_t i = 0; i < fh.entity_count; ++i) {
        const auto& e = cmd.entities[i];
        EntitySnapshot snap{};
        snap.entity_id = e.entity_id;
        snap.type      = e.entity_type;
        snap.flags     = 0;
        std::memcpy(snap.position,    e.position,    sizeof(snap.position));
        std::memcpy(snap.orientation, e.orientation, sizeof(snap.orientation));
        std::memcpy(snap.velocity,    e.velocity,    sizeof(snap.velocity));
        snap.health    = e.health;
        std::memcpy(snap.callsign,    e.callsign,    sizeof(snap.callsign));
        file_.write(reinterpret_cast<const char*>(&snap), sizeof(snap));
    }

    frames_written_.fetch_add(1, std::memory_order_relaxed);
}

void Recorder::finalise() {
    if (!file_) return;

    // Write frame index.
    uint64_t index_offset = static_cast<uint64_t>(file_.tellp());
    file_.write(reinterpret_cast<const char*>(frame_index_.data()),
                static_cast<std::streamsize>(frame_index_.size() * sizeof(IndexEntry)));

    // Write footer.
    FileFooter footer{};
    footer.index_offset = index_offset;
    footer.magic        = kFileMagic;
    file_.write(reinterpret_cast<const char*>(&footer), sizeof(footer));

    // Rewind and patch the file header with final stats.
    auto now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    header_.end_time_ns   = now_ns;
    header_.frame_count   = frames_written_.load();
    header_.index_offset  = index_offset;

    file_.seekp(0);
    file_.write(reinterpret_cast<const char*>(&header_), sizeof(header_));
    file_.flush();
    file_.close();
}

// ── Dashcam save ─────────────────────────────────────────────────────────────
bool Recorder::export_slice(const TelemetryStore& store,
                            float duration_seconds,
                            const std::filesystem::path& out_path,
                            const std::string& session_name) noexcept
{
    auto snapshots = store.extract_last(duration_seconds);
    if (snapshots.empty()) return false;

    std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f) return false;

    FileHeader hdr{};
    hdr.magic         = kFileMagic;
    hdr.version       = kFileVersion;
    hdr.start_time_ns = snapshots.front().timestamp_ns;
    hdr.end_time_ns   = snapshots.back().timestamp_ns;
    hdr.frame_count   = snapshots.size();

    auto n = std::min(session_name.size(), size_t(31));
    std::memcpy(hdr.session_name, session_name.c_str(), n);

    const SceneOrigin origin{};
    f.write(reinterpret_cast<const char*>(&hdr),    sizeof(hdr));
    f.write(reinterpret_cast<const char*>(&origin), sizeof(origin));

    std::vector<IndexEntry> idx;

    for (size_t fi = 0; fi < snapshots.size(); ++fi) {
        const auto& snap = snapshots[fi];

        if (fi % kIndexInterval == 0)
            idx.push_back({snap.timestamp_ns, static_cast<uint64_t>(f.tellp())});

        FrameHeader fh{};
        fh.timestamp_ns = snap.timestamp_ns;
        fh.entity_count = static_cast<uint16_t>(snap.entities.size());
        fh.frame_bytes  = sizeof(FrameHeader) + fh.entity_count * sizeof(EntitySnapshot);
        f.write(reinterpret_cast<const char*>(&fh), sizeof(fh));

        for (const auto& e : snap.entities) {
            EntitySnapshot es{};
            es.entity_id = e.entity_id;
            es.type      = e.entity_type;
            es.flags     = 0;
            std::memcpy(es.position,    e.position,    sizeof(es.position));
            std::memcpy(es.orientation, e.orientation, sizeof(es.orientation));
            std::memcpy(es.velocity,    e.velocity,    sizeof(es.velocity));
            es.health    = e.health;
            std::memcpy(es.callsign,    e.callsign,    sizeof(es.callsign));
            f.write(reinterpret_cast<const char*>(&es), sizeof(es));
        }
    }

    hdr.index_offset = static_cast<uint64_t>(f.tellp());
    f.write(reinterpret_cast<const char*>(idx.data()),
            static_cast<std::streamsize>(idx.size() * sizeof(IndexEntry)));

    FileFooter footer{hdr.index_offset, kFileMagic};
    f.write(reinterpret_cast<const char*>(&footer), sizeof(footer));

    // Patch header.
    f.seekp(0);
    f.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
    return true;
}

// ── .dbr loader ──────────────────────────────────────────────────────────────
bool Recorder::load_into(const std::filesystem::path& path,
                         TelemetryStore& store) noexcept
{
    store.clear();

    std::ifstream f(path, std::ios::binary);
    if (!f) return false;

    FileHeader hdr{};
    f.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (!f || hdr.magic != kFileMagic) return false;
    // v2 widened the callsign field (8 -> 32), changing EntitySnapshot size.
    // Older recordings have an incompatible layout, so reject them rather than
    // reading garbage.
    if (hdr.version != kFileVersion) return false;

    SceneOrigin origin{};
    f.read(reinterpret_cast<char*>(&origin), sizeof(origin));

    // Read frames sequentially up to index_offset (or EOF if index not written).
    uint64_t data_end = hdr.index_offset ? hdr.index_offset : UINT64_MAX;

    while (f && static_cast<uint64_t>(f.tellg()) < data_end) {
        FrameHeader fh{};
        f.read(reinterpret_cast<char*>(&fh), sizeof(fh));
        if (!f) break;

        std::vector<net::EntityState> entities;
        entities.reserve(fh.entity_count);

        for (uint16_t i = 0; i < fh.entity_count; ++i) {
            EntitySnapshot snap{};
            f.read(reinterpret_cast<char*>(&snap), sizeof(snap));
            if (!f) break;

            net::EntityState s{};
            s.entity_id   = snap.entity_id;
            s.entity_type = snap.type;
            s.health      = snap.health;
            s.timestamp_ns= fh.timestamp_ns;
            std::memcpy(s.position,    snap.position,    sizeof(s.position));
            std::memcpy(s.orientation, snap.orientation, sizeof(s.orientation));
            std::memcpy(s.velocity,    snap.velocity,    sizeof(s.velocity));
            std::memcpy(s.callsign,    snap.callsign,    sizeof(s.callsign));
            entities.push_back(s);
        }

        store.ingest(fh.timestamp_ns, entities);
    }

    store.rebuild_entity_histories();
    return true;
}

} // namespace debrief::persist
