#pragma once
#include "AssetManager.hpp"
#include <unordered_map>

namespace afteraction {

// ─────────────────────────────────────────────────────────────────────────────
//  Built-in models, loosely after Turkish platforms (TUSAŞ where possible),
//  at real size in metres. Axes: nose → −Z, up → +Y, right wing → +X.
//
//    TYPE_JET     KAAN-style 5th-gen fighter (twin canted tails, chined nose)
//    TYPE_HELO    T129 ATAK-style attack helicopter (spinning main/tail rotor)
//    TYPE_UAV     ANKA-style MALE UAV (spinning pusher propeller)
//    TYPE_UCAV    ANKA-3-style flying-wing UCAV
//    TYPE_SAM     HİSAR-O-style SAM launcher (6×6 truck, raised canisters)
//    TYPE_PROP    HÜRKUŞ-style turboprop trainer (spinning 4-blade propeller)
//    TYPE_TRANSPORT C-130-style transport (four spinning propellers)
//    TYPE_MISSILE air-to-air missile
//    TYPE_AAA     KORKUT-style tracked twin-gun air-defence vehicle
//    TYPE_GROUND  6×6 armoured vehicle
//    TYPE_SHIP    MİLGEM (Ada-class) corvette
//    other        diamond marker
// ─────────────────────────────────────────────────────────────────────────────
void build_procedural_models(std::unordered_map<uint16_t, ModelEntry>& out, ModelEntry& fallback);

} // namespace afteraction
