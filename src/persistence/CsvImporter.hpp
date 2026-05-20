#pragma once
#include "../network/Packet.hpp"
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace debrief::persist {

// ─────────────────────────────────────────────────────────────────────────────
//  CSV Importer
//
//  Reads telemetry from CSV files with user-defined column mapping.
//  Useful for importing logs from flight simulators, DCS World, ACMI tools,
//  or custom data sources.
//
//  Example mapping:
//    CsvImporter importer;
//    importer.map("time",       CsvField::TimestampSec);
//    importer.map("entity_id",  CsvField::EntityId);
//    importer.map("x",          CsvField::PosX);
//    importer.map("y",          CsvField::PosY);
//    importer.map("alt",        CsvField::PosZ);
//    importer.map("heading",    CsvField::YawDeg);
//    auto frames = importer.import("log.csv");
// ─────────────────────────────────────────────────────────────────────────────

enum class CsvField {
    Ignore,
    TimestampSec,   // seconds (float) → converted to ns
    TimestampMs,    // milliseconds
    TimestampNs,    // nanoseconds (uint64)
    EntityId,
    EntityType,
    SourceId,
    PosX, PosY, PosZ,
    VelX, VelY, VelZ,
    YawDeg, PitchDeg, RollDeg,
    Health,
    Callsign,
};

class CsvImporter {
public:
    // Column header name → semantic field.
    void map(const std::string& column_name, CsvField field) {
        mappings_[column_name] = field;
    }

    // Delimiter (comma by default; supports tab-separated, semicolon, etc.).
    char delimiter = ',';

    // If true, the first row is treated as a header and used for column mapping.
    bool has_header = true;

    // Fallback source_id when the CSV has no source column.
    uint16_t default_source_id = 0;

    // Callback invoked for each parsed row — allows streaming without storing all rows.
    using RowCallback = std::function<void(const net::EntityState&)>;

    // Parse the CSV and call `cb` for each valid row.
    // Returns the number of rows successfully parsed.
    size_t import(const std::filesystem::path& path, RowCallback cb) const noexcept;

    // Convenience: collect all states into a vector.
    std::vector<net::EntityState> import_all(const std::filesystem::path& path) const noexcept {
        std::vector<net::EntityState> result;
        import(path, [&](const net::EntityState& s) { result.push_back(s); });
        return result;
    }

private:
    std::unordered_map<std::string, CsvField> mappings_;

    std::vector<std::string> split_line(const std::string& line) const;
    std::optional<net::EntityState> parse_row(
        const std::vector<std::string>& headers,
        const std::vector<std::string>& values,
        uint64_t& base_time_ns) const noexcept;

    static float deg_to_rad(float deg) noexcept { return deg * 0.017453293f; }
};

} // namespace debrief::persist
