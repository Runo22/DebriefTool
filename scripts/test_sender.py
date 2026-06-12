#!/usr/bin/env python3
"""
AFTERACTION  —  UDP test packet sender
Sends a simple flight scenario (2 jets + 1 missile) to the afteraction application.

Usage:
    python test_sender.py [--host HOST] [--port PORT] [--hz HZ]

Requirements: Python 3.8+, no external packages.
"""

import argparse
import math
import socket
import struct
import time

# ── Wire format (must match src/network/Packet.hpp) ───────────────────────────
# BatchHeader:  4s B B I            = magic, count, source_id, sequence (uint32)
# EntityUpdate: I H B 32s ddd ddd d Q
#               id type health callsign  lat lon alt  phi theta psi  speed time_ns
# All angle/position/speed fields are doubles. Callsign holds up to 31 chars.

MAGIC = b'DBF1'
HDR_FMT  = '<4sBBI'             # 10 bytes
ENT_FMT  = '<IHB32sdddddddQ'    # 103 bytes
CALLSIGN_LEN = 32

TYPE_JET     = 1
TYPE_MISSILE = 2
TYPE_GROUND  = 4

def make_entity(eid, etype, callsign, lat, lon, alt,
                phi=0.0, theta=0.0, psi=0.0, speed=0.0, health=255):
    cs = callsign.encode()[:CALLSIGN_LEN - 1].ljust(CALLSIGN_LEN, b'\x00')
    return struct.pack(ENT_FMT,
        eid, etype, health, cs,
        lat, lon, alt,
        phi, theta, psi,
        speed,
        time.time_ns())

def make_packet(entities, source_id=0, seq=0):
    hdr = struct.pack(HDR_FMT, MAGIC, len(entities), source_id, seq & 0xFFFFFFFF)
    return hdr + b''.join(entities)

# ── Scenario helpers ──────────────────────────────────────────────────────────
# Scene centred over eastern Mediterranean — change to wherever you like.
ORIGIN_LAT = 36.85
ORIGIN_LON = 35.12

def enu_to_latlon(x_m, z_m):
    """Convert ENU offset (metres) back to lat/lon for the test sender."""
    cos_lat = math.cos(math.radians(ORIGIN_LAT))
    lat = ORIGIN_LAT - z_m / 111319.9
    lon = ORIGIN_LON + x_m / (111319.9 * cos_lat)
    return lat, lon

def orbit(cx, cz, radius, omega, t):
    """Clockwise orbit. Returns (x, z, psi_deg)."""
    theta = omega * t
    x = cx + radius * math.sin(theta)
    z = cz - radius * math.cos(theta)
    psi = math.degrees(theta) + 90.0
    return x, z, psi % 360.0

# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=22522)
    ap.add_argument('--hz',   type=float, default=10.0, help='Update rate (default 10 Hz)')
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    target = (args.host, args.port)
    interval = 1.0 / args.hz
    seq = 0
    t0  = time.monotonic()

    print(f"Sending to {args.host}:{args.port} at {args.hz} Hz  (Ctrl-C to stop)")
    print(f"Origin: lat={ORIGIN_LAT}, lon={ORIGIN_LON}")

    missile_active = False
    mx, my, mz = 0.0, 3000.0, 0.0

    while True:
        t = time.monotonic() - t0
        entities = []

        # ── VIPER01  (blue jet, CW orbit, 3 000 m, R=5 000 m, 200 m/s) ───────
        speed1 = 200.0
        r1, alt1 = 5000.0, 3000.0
        x1, z1, psi1 = orbit(0, 0, r1, speed1 / r1, t)
        lat1, lon1 = enu_to_latlon(x1, z1)
        entities.append(make_entity(1, TYPE_JET, 'F01', lat1, lon1, alt1,
                                    phi=22.0, psi=psi1, speed=speed1))

        # ── VIPER02  (red jet, CCW orbit, 4 000 m, R=7 000 m, 250 m/s) ───────
        speed2 = 250.0
        r2, alt2 = 7000.0, 4000.0
        x2, z2, psi2 = orbit(1000, -500, r2, -(speed2 / r2), t + 0.8)
        psi2 = (psi2 + 180.0) % 360.0
        lat2, lon2 = enu_to_latlon(x2, z2)
        entities.append(make_entity(2, TYPE_JET, 'F02', lat2, lon2, alt2,
                                    phi=-22.0, psi=psi2, speed=speed2))

        # ── AIM-120  (missile, launches at t=15 s) ────────────────────────────
        if t >= 15.0:
            if not missile_active:
                mx, mz = x1, z1
                my = alt1
                missile_active = True

            dx, dy, dz = x2 - mx, alt2 - my, z2 - mz
            dist = math.sqrt(dx*dx + dy*dy + dz*dz) + 0.001
            step = 500.0 * interval
            mx += dx / dist * step
            my += dy / dist * step
            mz += dz / dist * step

            mpsi  = math.degrees(math.atan2(dx, -dz)) % 360.0
            mtheta= math.degrees(math.asin(max(-1.0, min(1.0, dy / dist))))

            mlat, mlon = enu_to_latlon(mx, mz)
            entities.append(make_entity(3, TYPE_MISSILE, 'AIM', mlat, mlon, my,
                                        theta=mtheta, psi=mpsi, speed=500.0))

        # ── BRAVO1  (static AAA) ─────────────────────────────────────────────
        blat, blon = enu_to_latlon(2000, -3000)
        entities.append(make_entity(4, TYPE_GROUND, 'BRAVO', blat, blon, 0.0))

        pkt = make_packet(entities, seq=seq)
        sock.sendto(pkt, target)
        print(f"\r  t={t:6.1f}s  seq={seq:5d}  pktsz={len(pkt)} B", end='', flush=True)
        seq += 1

        time.sleep(interval)

if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped.")
