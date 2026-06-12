#include "PacketParser.hpp"
#include <chrono>
#include <cstring>

namespace afteraction::net {

std::expected<ParsedFrame, ParseError>
PacketParser::parse(std::span<const std::byte> buf) noexcept
{
    if (buf.size() < sizeof(BatchHeader))
        return std::unexpected{ParseError::TooShort};

    BatchHeader hdr;
    std::memcpy(&hdr, buf.data(), sizeof(hdr));

    if (hdr.magic[0] != kMagic[0] || hdr.magic[1] != kMagic[1] ||
        hdr.magic[2] != kMagic[2] || hdr.magic[3] != kMagic[3])
        return std::unexpected{ParseError::BadMagic};

    const size_t payload_needed = sizeof(BatchHeader) + hdr.count * sizeof(EntityUpdate);
    if (buf.size() < payload_needed)
        return std::unexpected{ParseError::CountOverflow};

    const uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    ParsedFrame frame;
    frame.source_id = hdr.source_id;
    frame.sequence  = hdr.sequence;
    frame.entities.reserve(hdr.count);

    const std::byte* ptr = buf.data() + sizeof(BatchHeader);

    for (uint8_t i = 0; i < hdr.count; ++i) {
        EntityUpdate wire;
        std::memcpy(&wire, ptr, sizeof(wire));
        ptr += sizeof(EntityUpdate);

        EntityState s;
        s.source_id    = hdr.source_id;
        s.entity_id    = wire.id;
        s.entity_type  = wire.type;
        s.timestamp_ns = (wire.time_ns == 0) ? now_ns : wire.time_ns;
        s.health       = wire.health;
        s.destroyed    = (wire.health == 0);

        // Callsign — copy and ensure null-termination
        std::memcpy(s.callsign, wire.callsign, sizeof(wire.callsign));
        s.callsign[kCallsignLen - 1] = '\0';

        s.lat_deg   = wire.lat;
        s.lon_deg   = wire.lon;
        s.alt_m     = wire.alt;
        s.phi_deg   = wire.phi;
        s.theta_deg = wire.theta;
        s.psi_deg   = wire.psi;
        s.speed_mps = wire.speed;

        // position[] and orientation[] are left zero here.
        // Application::apply_state_to_ecs() fills them using the scene origin.

        frame.entities.push_back(s);
    }

    return frame;
}

} // namespace afteraction::net
