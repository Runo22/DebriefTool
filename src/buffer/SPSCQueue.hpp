#pragma once
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <optional>

// Single-Producer Single-Consumer lock-free ring queue.
//
// Hot path: one atomic load + one atomic store per push/pop.  No mutexes,
// no CAS loops.  Cache-line padding on the two indexes prevents false sharing
// between the producer and consumer cores (critical on x86 where two sockets
// could be involved).
//
// Capacity must be a power of two so the modulo becomes a bitmask.
template<typename T, size_t N>
class SPSCQueue {
    static_assert(std::has_single_bit(N), "SPSCQueue capacity must be a power of 2");
    static constexpr size_t kMask = N - 1;

public:
    // Called from the PRODUCER thread only.
    [[nodiscard]] bool try_push(T value) noexcept {
        const auto w = write_.load(std::memory_order_relaxed);
        const auto r = read_.load(std::memory_order_acquire);
        if (w - r == N) return false;  // full
        slots_[w & kMask] = std::move(value);
        write_.store(w + 1, std::memory_order_release);
        return true;
    }

    // Called from the CONSUMER thread only.
    [[nodiscard]] std::optional<T> try_pop() noexcept {
        const auto r = read_.load(std::memory_order_relaxed);
        const auto w = write_.load(std::memory_order_acquire);
        if (r == w) return std::nullopt;  // empty
        T val = std::move(slots_[r & kMask]);
        read_.store(r + 1, std::memory_order_release);
        return val;
    }

    // Approximate — not synchronized, for diagnostics only.
    [[nodiscard]] size_t size_approx() const noexcept {
        return write_.load(std::memory_order_relaxed) -
               read_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool empty_approx() const noexcept { return size_approx() == 0; }

private:
    alignas(64) std::atomic<uint64_t> write_{ 0 };
    alignas(64) std::atomic<uint64_t> read_ { 0 };
    std::array<T, N> slots_{};
};
