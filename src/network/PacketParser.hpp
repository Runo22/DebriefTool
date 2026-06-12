#pragma once
#include "Packet.hpp"
#include <expected>
#include <span>

namespace afteraction::net {

enum class ParseError : uint8_t {
    TooShort,
    BadMagic,
    CountOverflow,   // count * sizeof(EntityUpdate) exceeds buffer
};

class PacketParser {
public:
    // Parses a raw UDP payload.
    // lat/lon/euler fields are filled verbatim.
    // position[] and orientation[] are left zero — Application fills them.
    [[nodiscard]]
    static std::expected<ParsedFrame, ParseError>
    parse(std::span<const std::byte> buf) noexcept;
};

} // namespace afteraction::net
