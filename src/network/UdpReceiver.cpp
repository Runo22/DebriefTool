#include "UdpReceiver.hpp"
#include "PacketParser.hpp"
#include <array>
#include <cstring>

#ifdef _WIN32
#  pragma comment(lib, "ws2_32.lib")
#endif

namespace afteraction::net {

namespace {
#ifdef _WIN32
    struct WsaGuard {
        WsaGuard()  { WSADATA d; WSAStartup(MAKEWORD(2,2), &d); }
        ~WsaGuard() { WSACleanup(); }
    };
    WsaGuard g_wsa; // initialised at static init time

    void close_fd(SocketHandle s) { closesocket(s); }
    int  last_error()             { return WSAGetLastError(); }
    bool would_block(int e)       { return e == WSAEWOULDBLOCK || e == WSAETIMEDOUT; }
#else
    void close_fd(SocketHandle s) { ::close(s); }
    int  last_error()             { return errno; }
    bool would_block(int e)       { return e == EAGAIN || e == EWOULDBLOCK; }
#endif
} // anon namespace

UdpReceiver::UdpReceiver(SPSCQueue<ParsedFrame, 4096>& queue) : queue_(queue) {}

UdpReceiver::~UdpReceiver() { stop(); }

bool UdpReceiver::start(const std::string& addr, uint16_t port) noexcept {
    if (running_.load()) return false;
    if (!open_socket(addr, port)) return false;
    running_.store(true, std::memory_order_relaxed);
    thread_ = std::thread(&UdpReceiver::recv_loop, this);
    return true;
}

void UdpReceiver::stop() noexcept {
    running_.store(false, std::memory_order_relaxed);
    close_socket();
    if (thread_.joinable()) thread_.join();
}

bool UdpReceiver::open_socket(const std::string& addr, uint16_t port) noexcept {
    sock_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == kInvalidSocket) return false;

    // Set a receive timeout so the loop can check the stop flag periodically.
#ifdef _WIN32
    DWORD tv = 100; // ms
    ::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{ .tv_sec = 0, .tv_usec = 100'000 };
    ::setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    // Enlarge socket receive buffer to absorb bursts (4 MiB).
    int rcvbuf = 4 * 1024 * 1024;
    ::setsockopt(sock_, SOL_SOCKET, SO_RCVBUF,
                 reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

    sockaddr_in sa{};
    sa.sin_family      = AF_INET;
    sa.sin_port        = htons(port);
    sa.sin_addr.s_addr = (addr == "0.0.0.0" || addr.empty())
                         ? INADDR_ANY
                         : ::inet_addr(addr.c_str());

    if (::bind(sock_, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
        close_socket();
        return false;
    }
    return true;
}

void UdpReceiver::close_socket() noexcept {
    if (sock_ != kInvalidSocket) {
        close_fd(sock_);
        sock_ = kInvalidSocket;
    }
}

void UdpReceiver::recv_loop() {
    // Stack-allocate the max UDP payload buffer (64 KiB).
    // Avoids heap allocation in the hot path.
    std::array<std::byte, 65507> raw{};

    while (running_.load(std::memory_order_relaxed)) {
        const int n = static_cast<int>(
            ::recv(sock_, reinterpret_cast<char*>(raw.data()),
                   static_cast<int>(raw.size()), 0));

        if (n <= 0) {
            // Timeout or error — just loop back and check running_.
            continue;
        }

        stats_.bytes_received += static_cast<uint64_t>(n);
        ++stats_.packets_received;

        auto result = PacketParser::parse(
            std::span<const std::byte>{raw.data(), static_cast<size_t>(n)});

        if (!result) {
            ++stats_.parse_errors;
            continue;
        }

        ParsedFrame& frame = *result;

        // Sequence gap detection per source (recv thread only — no lock).
        // `it->second` tracks the highest in-order sequence seen. The signed
        // 32-bit difference interprets wrap correctly (e.g. 0xFFFFFFFF -> 0 is +1):
        //   diff == 1  : the expected next packet — advance.
        //   diff  > 1  : forward jump — (diff-1) packets were dropped.
        //   diff == 0  : duplicate — ignore.
        //   diff  < 0  : a late/out-of-order packet — ignore WITHOUT rewinding the
        //                tracker, so a following in-order packet isn't miscounted.
        //   diff much < 0 : far in the past ⇒ the sender restarted — resync.
        // This avoids the false gaps that arise from moving the tracker backwards.
        constexpr int32_t kReorderWindow = 4096; // tolerated out-of-order distance
        auto [it, inserted] = last_seq_.emplace(frame.source_id, frame.sequence);
        if (!inserted) {
            const int32_t diff = static_cast<int32_t>(frame.sequence - it->second);
            if (diff == 1) {
                it->second = frame.sequence;
            } else if (diff > 1) {
                stats_.sequence_gaps += static_cast<uint64_t>(diff - 1);
                it->second = frame.sequence;
            } else if (diff < -kReorderWindow) {
                it->second = frame.sequence;   // sender restart — resync, no gap
            }
            // diff == 0 (duplicate) or small-negative (reorder): keep highest seq.
        }

        // Non-blocking push; if queue is full the main thread is lagging.
        if (!queue_.try_push(std::move(frame)))
            ++stats_.packets_dropped;
    }
}

} // namespace afteraction::net
