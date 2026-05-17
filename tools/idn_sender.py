#!/usr/bin/env python3
"""IDN-Hello / IDN-RT sender for ILDAWave DACs.

This script has the same user-facing test commands as ilw_sender.py, but sends
points through the ILDA IDN realtime UDP protocol on port 7255.

Quick start:

  Draw a circle for 5 seconds at 30 kpps:
    python idn_sender.py --host 192.168.1.50 --pps 30000 circle --seconds 5

  Draw a square:
    python idn_sender.py --host 192.168.1.50 --pps 30000 --packet-points 64 square --seconds 10

  Stream an ILDA file at 30 FPS and loop it:
    python idn_sender.py --host 192.168.1.50 --pps 30000 --packet-points 64 ild show.ild --fps 30 --loop

  Stop and close the realtime session:
    python idn_sender.py --host 192.168.1.50 stop

The ESP32 firmware supports a simple default IDN sample layout:
X_hi X_lo Y_hi Y_lo R G B I. This sender uses that layout, so each point is
8 bytes and no tag-dictionary setup is needed for normal streaming.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time
from typing import Optional, Sequence

from ilw_sender import (
    LaserPoint,
    make_circle,
    make_sine,
    make_square,
    normalize_ilda_points,
    resample_points,
    load_ilda,
    u16_to_u8,
    wait_until_precise,
)


IDN_PORT = 7255

IDNCMD_PING_REQ = 0x08
IDNCMD_SCAN_REQ = 0x10
IDNCMD_SVCMAP_REQ = 0x12
IDNCMD_RT_CHANNEL = 0x40
IDNCMD_RT_CHANNEL_ACK = 0x41
IDNCMD_RT_CLOSE = 0x44

CHUNK_WAVE = 0x01


def clamp_i16(v) -> int:
    return max(-32768, min(32767, int(round(v))))


def be24(v: int) -> bytes:
    v = max(0, min(0xFFFFFF, int(v)))
    return bytes([(v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF])


class IDNSender:
    """Small IDN-RT UDP sender for realtime point streaming."""

    def __init__(self, host: str, port: int = IDN_PORT, bind: Optional[str] = None, quiet: bool = False):
        self.addr = (host, port)
        self.quiet = quiet
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.settimeout(0.25)
        try:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
        except OSError:
            pass
        if bind:
            self.sock.bind((bind, 0))
        self.seq = 1
        self.timestamp_us = 0

    def close_socket(self) -> None:
        self.sock.close()

    def next_seq(self) -> int:
        seq = self.seq & 0xFFFF
        self.seq = (self.seq + 1) & 0xFFFF
        return seq

    def send_hello(self, cmd: int, payload: bytes = b"", flags: int = 0, seq: Optional[int] = None) -> None:
        if seq is None:
            seq = self.next_seq()
        self.sock.sendto(struct.pack(">BBH", cmd, flags & 0xFF, seq & 0xFFFF) + payload, self.addr)

    def ping(self) -> None:
        self.send_hello(IDNCMD_PING_REQ)

    def scan(self) -> None:
        self.send_hello(IDNCMD_SCAN_REQ)

    def svcmap(self) -> None:
        self.send_hello(IDNCMD_SVCMAP_REQ)

    def stop(self) -> None:
        # Empty RT_CLOSE closes the active session in the ESP firmware.
        self.send_hello(IDNCMD_RT_CLOSE)

    def encode_sample(self, p: LaserPoint) -> bytes:
        """Encode one point into the firmware's default IDN sample layout."""
        p = p.with_blanking_applied()
        x = clamp_i16(p.x) & 0xFFFF
        y = clamp_i16(p.y) & 0xFFFF
        return bytes([
            (x >> 8) & 0xFF,
            x & 0xFF,
            (y >> 8) & 0xFF,
            y & 0xFF,
            u16_to_u8(p.r),
            u16_to_u8(p.g),
            u16_to_u8(p.b),
            u16_to_u8(p.i),
        ])

    def send_points_packet(self, points: Sequence[LaserPoint], pps: int, channel: int = 0) -> None:
        if not points:
            return

        samples = b"".join(self.encode_sample(p) for p in points)

        # Duration is how the ESP firmware derives the output point rate.
        duration_us = max(1, int(round(len(points) * 1000000.0 / float(pps))))
        wave_chunk = b"\x00" + be24(duration_us) + samples

        # IDN-Stream channel message header:
        # total_size(2), CNL(1), chunk_type(1), timestamp_us(4).
        # CNL bit7 marks channel message. No config flag is needed because the
        # ESP firmware auto-opens the channel with the default sample layout.
        total_size = 8 + len(wave_chunk)
        cnl = 0x80 | (channel & 0x3F)
        stream_header = struct.pack(">HBBI", total_size, cnl, CHUNK_WAVE, self.timestamp_us & 0xFFFFFFFF)

        self.send_hello(IDNCMD_RT_CHANNEL, stream_header + wave_chunk)
        self.timestamp_us = (self.timestamp_us + duration_us) & 0xFFFFFFFF

    def stream_points(
        self,
        points: Sequence[LaserPoint],
        pps: int,
        seconds: Optional[float] = None,
        loop=True,
        points_per_packet: int = 64,
        stop_on_exit=True,
    ) -> None:
        if not points:
            raise ValueError("No points to stream")
        if pps <= 0:
            raise ValueError("pps must be > 0")

        points_per_packet = max(1, min(180, int(points_per_packet)))
        packet_interval = points_per_packet / float(pps)
        start = time.perf_counter()
        next_send = start
        idx = 0
        total = 0

        try:
            while True:
                if seconds is not None and time.perf_counter() - start >= seconds:
                    break

                batch = []
                for _ in range(points_per_packet):
                    if idx >= len(points):
                        if not loop:
                            break
                        idx = 0
                    batch.append(points[idx])
                    idx += 1
                if not batch:
                    break

                self.send_points_packet(batch, pps)
                total += len(batch)

                next_send += packet_interval
                delay = next_send - time.perf_counter()
                if delay > 0:
                    wait_until_precise(next_send)
                elif delay < -0.25:
                    next_send = time.perf_counter()
        finally:
            if stop_on_exit:
                self.stop()
            if not self.quiet:
                elapsed = max(0.001, time.perf_counter() - start)
                print(f"Streamed {total} points in {elapsed:.2f}s ({total / elapsed:.0f} pps)")


def stream_ilda_file(sender: IDNSender, path, pps, fps=30.0, loop=False, scale=1.0, points_per_packet=64) -> None:
    frames = load_ilda(path)
    if not frames:
        raise RuntimeError(f"No supported ILDA frames found in {path}")

    frame_points = max(1, int(round(pps / fps))) if fps > 0 else 0
    idx = 0

    try:
        while True:
            pts = normalize_ilda_points(frames[idx].points, scale=scale)
            if frame_points > 0:
                pts = resample_points(pts, frame_points)
            sender.stream_points(pts, pps=pps, seconds=None, loop=False, points_per_packet=points_per_packet, stop_on_exit=False)
            idx += 1
            if idx >= len(frames):
                if not loop:
                    break
                idx = 0
    finally:
        sender.stop()


def build_arg_parser():
    examples = """examples:
  python idn_sender.py --host 192.168.1.50 circle --seconds 5
  python idn_sender.py --host 192.168.1.50 --packet-points 64 square --seconds 10
  python idn_sender.py --host 192.168.1.50 --pps 30000 ild show.ild --fps 30 --loop
  python idn_sender.py --host 192.168.1.50 stop
"""
    p = argparse.ArgumentParser(
        description="IDN-Hello / IDN-RT sender with ILW-style test commands",
        epilog=examples,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--host", required=True, help="DAC IP address")
    p.add_argument("--port", type=int, default=IDN_PORT, help=f"UDP port, default {IDN_PORT}")
    p.add_argument("--pps", type=int, default=30000, help="Point output rate. Default: 30000")
    p.add_argument("--packet-points", type=int, default=64, help="Points per UDP packet. Default: 64")
    p.add_argument("--bind", default=None, help="Optional local source IP to bind before sending")
    p.add_argument("--quiet", action="store_true")

    sub = p.add_subparsers(dest="cmd", required=True)
    sq = sub.add_parser("square", help="draw a square test pattern")
    sq.add_argument("--seconds", type=float, default=5.0)
    sq.add_argument("--size", type=int, default=22000)
    sq.add_argument("--points-per-side", type=int, default=80)

    ci = sub.add_parser("circle", help="draw a color circle test pattern")
    ci.add_argument("--seconds", type=float, default=5.0)
    ci.add_argument("--radius", type=int, default=22000)
    ci.add_argument("--points", type=int, default=360)

    si = sub.add_parser("sine", help="draw a sine-wave test pattern")
    si.add_argument("--seconds", type=float, default=5.0)
    si.add_argument("--width", type=int, default=26000)
    si.add_argument("--height", type=int, default=16000)
    si.add_argument("--points", type=int, default=400)
    si.add_argument("--cycles", type=float, default=2.0)

    ild = sub.add_parser("ild", help="stream an ILDA file")
    ild.add_argument("path")
    ild.add_argument("--fps", type=float, default=30.0)
    ild.add_argument("--loop", action="store_true")
    ild.add_argument("--scale", type=float, default=1.0)

    sub.add_parser("stop", help="close realtime session and blank by firmware policy")
    sub.add_parser("scan", help="send IDN scan request")
    sub.add_parser("svcmap", help="send IDN service-map request")
    return p


def main():
    args = build_arg_parser().parse_args()
    sender = IDNSender(args.host, args.port, bind=args.bind, quiet=args.quiet)
    try:
        if args.cmd == "stop":
            sender.stop()
            return 0
        if args.cmd == "scan":
            sender.scan()
            return 0
        if args.cmd == "svcmap":
            sender.svcmap()
            return 0
        if args.cmd == "square":
            pts = make_square(args.size, args.points_per_side)
            sender.stream_points(pts, args.pps, args.seconds, True, args.packet_points)
            return 0
        if args.cmd == "circle":
            pts = make_circle(args.radius, args.points)
            sender.stream_points(pts, args.pps, args.seconds, True, args.packet_points)
            return 0
        if args.cmd == "sine":
            pts = make_sine(args.width, args.height, args.points, args.cycles)
            sender.stream_points(pts, args.pps, args.seconds, True, args.packet_points)
            return 0
        if args.cmd == "ild":
            stream_ilda_file(sender, args.path, args.pps, args.fps, args.loop, args.scale, args.packet_points)
            return 0
        raise RuntimeError("Unknown command")
    finally:
        sender.close_socket()


if __name__ == "__main__":
    raise SystemExit(main())
