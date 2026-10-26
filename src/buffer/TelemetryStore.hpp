#pragma once
#include "../network/Packet.hpp"
#include <algorithm>
#include <array>
#include <deque>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace afteraction {

// ─── Per-entity state history (lives as an ECS component) ─────────────────────
//
// Fixed-size ring buffer of timestamped states.  At 20 Hz update rate,
// 512 slots = ~25 seconds of per-entity history — enough for interpolation
// and short rewinds.  For longer scrubbing the full TelemetryStore is used.
struct EntityStateHistory {
    static constexpr uint32_t kCapacity = 512;

    struct Keyframe {
        uint64_t timestamp_ns = 0;
        float    position[3]  = {};
        float    orientation[4] = {0, 0, 0, 1};
        float    velocity[3]  = {};
        uint8_t  health       = 255;
    };

    std::array<Keyframe, kCapacity> frames{};
    uint32_t write_idx = 0;  // next write position (wraps via % kCapacity)
    uint32_t count     = 0;  // number of valid entries (≤ kCapacity)

    // Keeps frames in ascending time order (find_floor relies on it): a sample
    // older than the newest is dropped, unless it is more than 5 s older — then
    // the sender's clock restarted and the history starts over.
    void push(const Keyframe& kf) noexcept {
        if (count > 0) {
            const uint64_t newest = frames[(write_idx + kCapacity - 1) % kCapacity].timestamp_ns;
            if (kf.timestamp_ns <= newest) {
                if (kf.timestamp_ns + 5'000'000'000ULL >= newest) return;
                write_idx = 0;
                count = 0;
            }
        }
        frames[write_idx % kCapacity] = kf;
        ++write_idx;
        if (count < kCapacity) ++count;
    }

    // Returns the index (into `frames[]`) of the latest keyframe whose
    // timestamp ≤ `ts_ns`, or -1 if none exists.
    // Assumes frames are written in ascending timestamp order.
    [[nodiscard]] int find_floor(uint64_t ts_ns) const noexcept {
        if (count == 0) return -1;
        // The ring's "start" — oldest valid slot.
        uint32_t start = (count < kCapacity) ? 0 : (write_idx % kCapacity);
        // Binary search on logical indices [0, count).
        int lo = 0, hi = static_cast<int>(count) - 1, result = -1;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            uint32_t slot = (start + static_cast<uint32_t>(mid)) % kCapacity;
            if (frames[slot].timestamp_ns <= ts_ns) {
                result = static_cast<int>(slot);
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        return result;
    }

    // Returns {a, b} where a.timestamp ≤ ts_ns < b.timestamp for interpolation.
    // If ts_ns is before all history, returns {frames[0], frames[0]}.
    // If ts_ns is after all history, returns {latest, latest}.
    struct Bracket { const Keyframe* lo; const Keyframe* hi; };
    [[nodiscard]] Bracket bracket(uint64_t ts_ns) const noexcept {
        if (count == 0) return {&frames[0], &frames[0]};
        uint32_t start = (count < kCapacity) ? 0 : (write_idx % kCapacity);
        uint32_t last  = (start + count - 1) % kCapacity;
        int floor_idx  = find_floor(ts_ns);
        if (floor_idx < 0) return {&frames[start], &frames[start]};
        if (static_cast<uint32_t>(floor_idx) == last) return {&frames[last], &frames[last]};
        uint32_t next = (static_cast<uint32_t>(floor_idx) + 1) % kCapacity;
        return {&frames[static_cast<uint32_t>(floor_idx)], &frames[next]};
    }
};

// ─── Full session telemetry store  (dashcam circular buffer) ──────────────────
//
// Stores a complete snapshot of all entity states every `snapshot_interval_ms`
// milliseconds.  Keeps the last `kMaxSnapshots` snapshots in memory.
//
// Thread safety: snapshots are written by the main thread only.
// Reading for the "Save Last N Seconds" feature takes a shared_mutex read lock.
// The dashcam save is a rare, user-triggered event — lock contention is negligible.
class TelemetryStore {
public:
    static constexpr uint32_t kMaxSnapshots    = 18'000; // 15 min @ 20 Hz
    static constexpr uint32_t kSnapshotIntervalMs = 50; // 20 Hz snapshots

    struct Snapshot {
        uint64_t timestamp_ns = 0;
        std::vector<net::EntityState> entities;
    };

    // Called from the main thread after consuming the SPSC queue.
    // States are merged per entity until the next snapshot is due (20 Hz), so
    // entities that report at different moments are all kept — not only the
    // ones in the batch that happens to fall on the snapshot tick.
    void ingest(uint64_t now_ns, const std::vector<net::EntityState>& states) {
        for (const auto& es : states)
            pending_[(static_cast<uint64_t>(es.source_id) << 32) | es.entity_id] = es;
        // Time going backwards by more than 5 s: a new timeline (restarted
        // sender / new file) — take a snapshot right away.
        if (now_ns + 5'000'000'000ULL < last_snapshot_ns_) last_snapshot_ns_ = 0;
        if (now_ns < last_snapshot_ns_ ||
            now_ns - last_snapshot_ns_ < kSnapshotIntervalMs * 1'000'000ULL)
            return;
        write_snapshot(now_ns);
    }

    // Write out states still waiting for the next snapshot tick (call at the
    // end of loading a file, so its last moments aren't lost).
    void flush_pending() {
        if (pending_.empty()) return;
        uint64_t ts = last_snapshot_ns_;
        for (const auto& [key, es] : pending_) ts = std::max(ts, es.timestamp_ns);
        write_snapshot(ts);
    }

    // Reset the store, clearing all snapshots, indexes, and histories.
    void clear() {
        std::unique_lock lock(mu_);
        // Clear in place. Do NOT write `ring_ = {}` — that materialises a
        // value-initialised std::array temporary (~kMaxSnapshots * sizeof(Snapshot),
        // hundreds of KB) on the stack, which overflows it when clear() is called
        // from a deep call stack (e.g. the UI "Clear" button during rendering).
        for (auto& snap : ring_) {
            snap.timestamp_ns = 0;
            snap.entities.clear();
        }
        write_idx_ = 0;
        count_ = 0;
        last_snapshot_ns_ = 0;
        pending_.clear();
        entity_histories_.clear();
    }

    // Rebuild the histories map after loading a session file.
    void rebuild_entity_histories() {
        std::unique_lock lock(mu_);
        entity_histories_.clear();
        uint32_t start = (count_ < kMaxSnapshots) ? 0 : (write_idx_ % kMaxSnapshots);
        for (uint32_t i = 0; i < count_; ++i) {
            const auto& s = ring_[(start + i) % kMaxSnapshots];
            for (const auto& es : s.entities) {
                uint64_t key = (static_cast<uint64_t>(es.source_id) << 32) | es.entity_id;
                entity_histories_[key].push_back(es);
            }
        }
    }

    // Retrieve the entire historical path of a specific entity.
    [[nodiscard]] const std::deque<net::EntityState>* get_entity_history(uint32_t source_id, uint32_t entity_id) const {
        std::shared_lock lock(mu_);
        uint64_t key = (static_cast<uint64_t>(source_id) << 32) | entity_id;
        auto it = entity_histories_.find(key);
        if (it != entity_histories_.end())
            return &it->second;
        return nullptr;
    }

    // Get all unique keys in entity_histories_
    [[nodiscard]] std::vector<uint64_t> get_all_entity_keys() const {
        std::shared_lock lock(mu_);
        std::vector<uint64_t> keys;
        keys.reserve(entity_histories_.size());
        for (const auto& [k, v] : entity_histories_)
            keys.push_back(k);
        return keys;
    }

    // Returns a copy of the last `seconds` worth of snapshots.
    // Thread-safe — can be called from a save worker thread.
    [[nodiscard]] std::vector<Snapshot> extract_last(float seconds) const {
        std::shared_lock lock(mu_);
        if (count_ == 0) return {};

        if (!(seconds > 0.0f)) return {};
        const uint64_t newest = ring_[(write_idx_ - 1) % kMaxSnapshots].timestamp_ns;
        const double span = std::min(double(seconds) * 1e9, double(newest));   // no underflow
        const uint64_t cutoff_ns = newest - static_cast<uint64_t>(span);

        std::vector<Snapshot> result;
        result.reserve(std::min<size_t>(count_, static_cast<size_t>(std::min(seconds, 3600.0f) * 25)));
        uint32_t start = (count_ < kMaxSnapshots) ? 0 : (write_idx_ % kMaxSnapshots);
        for (uint32_t i = 0; i < count_; ++i) {
            const auto& s = ring_[(start + i) % kMaxSnapshots];
            if (s.timestamp_ns >= cutoff_ns)
                result.push_back(s);
        }
        return result;
    }

    // Returns a copy of ALL snapshots (for writing a full session file).
    [[nodiscard]] std::vector<Snapshot> extract_all() const {
        std::shared_lock lock(mu_);
        std::vector<Snapshot> result;
        result.reserve(count_);
        uint32_t start = (count_ < kMaxSnapshots) ? 0 : (write_idx_ % kMaxSnapshots);
        for (uint32_t i = 0; i < count_; ++i)
            result.push_back(ring_[(start + i) % kMaxSnapshots]);
        return result;
    }

    // Binary search — returns the snapshot index closest to `ts_ns`.
    [[nodiscard]] std::optional<const Snapshot*> find_nearest(uint64_t ts_ns) const {
        std::shared_lock lock(mu_);
        if (count_ == 0) return std::nullopt;
        uint32_t start = (count_ < kMaxSnapshots) ? 0 : (write_idx_ % kMaxSnapshots);
        int lo = 0, hi = static_cast<int>(count_) - 1, best = 0;
        uint64_t best_dist = UINT64_MAX;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            const auto& s = ring_[(start + static_cast<uint32_t>(mid)) % kMaxSnapshots];
            uint64_t dist = (s.timestamp_ns >= ts_ns)
                            ? s.timestamp_ns - ts_ns
                            : ts_ns - s.timestamp_ns;
            if (dist < best_dist) { best_dist = dist; best = mid; }
            if (s.timestamp_ns < ts_ns) lo = mid + 1;
            else                        hi = mid - 1;
        }
        return &ring_[(start + static_cast<uint32_t>(best)) % kMaxSnapshots];
    }

    [[nodiscard]] uint32_t count() const {
        std::shared_lock lock(mu_);
        return count_;
    }

    [[nodiscard]] std::pair<uint64_t, uint64_t> time_range_ns() const {
        std::shared_lock lock(mu_);
        if (count_ == 0) return {0, 0};
        uint32_t start = (count_ < kMaxSnapshots) ? 0 : (write_idx_ % kMaxSnapshots);
        uint32_t endIdx = (write_idx_ - 1) % kMaxSnapshots;
        return {ring_[start].timestamp_ns, ring_[endIdx].timestamp_ns};
    }

private:
    void write_snapshot(uint64_t ts_ns) {
        last_snapshot_ns_ = ts_ns;
        std::unique_lock lock(mu_);
        Snapshot& slot = ring_[write_idx_ % kMaxSnapshots];
        slot.timestamp_ns = ts_ns;
        slot.entities.clear();
        for (const auto& [key, es] : pending_) slot.entities.push_back(es);
        pending_.clear();
        ++write_idx_;
        if (count_ < kMaxSnapshots) ++count_;

        // Update per-entity histories
        for (const auto& es : slot.entities) {
            uint64_t key = (static_cast<uint64_t>(es.source_id) << 32) | es.entity_id;
            auto& hist = entity_histories_[key];
            hist.push_back(es);
            if (hist.size() > kMaxSnapshots) hist.pop_front();   // O(1), unlike vector::erase
        }
    }

    mutable std::shared_mutex mu_;
    std::array<Snapshot, kMaxSnapshots> ring_{};
    uint32_t write_idx_       = 0;
    uint32_t count_           = 0;
    uint64_t last_snapshot_ns_= 0;
    std::unordered_map<uint64_t, net::EntityState> pending_;   // newest state per entity since the last snapshot
    std::unordered_map<uint64_t, std::deque<net::EntityState>> entity_histories_;
};

} // namespace afteraction
