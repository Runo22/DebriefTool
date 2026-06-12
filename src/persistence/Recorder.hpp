#pragma once
#include "BinaryFormat.hpp"
#include "../network/Packet.hpp"
#include "../buffer/TelemetryStore.hpp"
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace afteraction::persist {

// ─────────────────────────────────────────────────────────────────────────────
//  Recorder
//
//  Writes entity state to disk in the .aar binary format.
//  Uses a dedicated I/O thread with a write queue to keep the main thread
//  from stalling on disk latency.
//
//  Usage:
//    recorder.start("session_2025.aar");
//    // each frame:
//    recorder.submit(timestamp_ns, source_id, entities);
//    recorder.stop();  // flushes, writes index, writes footer
//
//  "Save Last N Seconds" (dashcam):
//    recorder.save_dashcam(store, seconds, "dashcam.aar");
// ─────────────────────────────────────────────────────────────────────────────

class Recorder {
public:
    Recorder();
    ~Recorder();

    bool start(const std::filesystem::path& path,
               const std::string& session_name = "",
               const SceneOrigin& origin = {}) noexcept;

    // Submits a batch of entity states for recording.
    // Non-blocking — data is copied to a write queue.
    void submit(uint64_t timestamp_ns, uint16_t source_id,
                const std::vector<net::EntityState>& entities) noexcept;

    // Finishes recording: flushes queue, writes frame index, writes footer.
    void stop() noexcept;

    // Exports a time slice from `store` to a new file.  Blocking (intended
    // for user-triggered save, not the hot path).
    static bool export_slice(const TelemetryStore& store,
                             float duration_seconds,
                             const std::filesystem::path& out_path,
                             const std::string& session_name = "") noexcept;

    // Loads a .aar file back into a TelemetryStore for playback.
    static bool load_into(const std::filesystem::path& path,
                          TelemetryStore& store) noexcept;

    [[nodiscard]] bool        is_recording() const noexcept { return recording_.load(); }
    [[nodiscard]] uint64_t    frames_written() const noexcept { return frames_written_.load(); }

private:
    struct WriteCmd {
        uint64_t timestamp_ns = 0;
        uint16_t source_id    = 0;
        std::vector<net::EntityState> entities;
    };

    void io_thread_fn();
    void write_frame(const WriteCmd& cmd);
    void finalise();

    std::ofstream               file_;
    std::filesystem::path       path_;
    FileHeader                  header_{};
    std::vector<IndexEntry>     frame_index_;

    std::mutex                  queue_mu_;
    std::vector<WriteCmd>       write_queue_;
    std::vector<WriteCmd>       swap_queue_;  // used during flush

    std::thread                 io_thread_;
    std::atomic<bool>           recording_{ false };
    std::atomic<bool>           stop_requested_{ false };
    std::atomic<uint64_t>       frames_written_{ 0 };
};

} // namespace afteraction::persist
