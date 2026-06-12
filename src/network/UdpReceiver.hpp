#pragma once
#include "Packet.hpp"
#include "../buffer/SPSCQueue.hpp"
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <cstdint>
#include <unordered_map>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
   using SocketHandle = SOCKET;
   inline constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <unistd.h>
   using SocketHandle = int;
   inline constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace afteraction::net {

struct ReceiverStats {
    uint64_t packets_received  = 0;
    uint64_t packets_dropped   = 0;   // queue full — main thread too slow
    uint64_t parse_errors      = 0;
    uint64_t bytes_received    = 0;
    uint64_t sequence_gaps     = 0;   // detected reorders/drops per source
};

// Listens on a UDP port in a dedicated thread.
// Parsed frames are pushed into an SPSC queue owned by the caller (main thread).
// Designed to scale: create one UdpReceiver per interface/port pair; each
// pushes into the same shared queue — the source_id in each packet disambiguates.
class UdpReceiver {
public:
    explicit UdpReceiver(SPSCQueue<ParsedFrame, 4096>& queue);
    ~UdpReceiver();

    UdpReceiver(const UdpReceiver&)            = delete;
    UdpReceiver& operator=(const UdpReceiver&) = delete;

    // Opens the socket and starts the receive thread.
    // `bind_addr` = "0.0.0.0" to listen on all interfaces.
    bool start(const std::string& bind_addr, uint16_t port) noexcept;

    // Signals the thread to stop and joins it.
    void stop() noexcept;

    [[nodiscard]] bool      is_running()  const noexcept { return running_.load(std::memory_order_relaxed); }
    [[nodiscard]] ReceiverStats stats()   const noexcept { return stats_; }

private:
    void recv_loop();
    bool open_socket(const std::string& addr, uint16_t port) noexcept;
    void close_socket() noexcept;

    SPSCQueue<ParsedFrame, 4096>& queue_;
    SocketHandle  sock_   { kInvalidSocket };
    std::thread   thread_;
    std::atomic<bool> running_{ false };
    ReceiverStats stats_{};

    // Per-source sequence tracking for gap detection.
    // Key: source_id, Value: last sequence number.
    // Accessed only from the recv thread — no lock needed.
    std::unordered_map<uint16_t, uint32_t> last_seq_;
};

} // namespace afteraction::net
