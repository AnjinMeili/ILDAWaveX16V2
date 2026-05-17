#!/usr/bin/env python3
"""Ether Dream protocol sender for ILDAWave DACs.

This script has the same user-facing test commands as ilw_sender.py, but sends
them through the Ether Dream TCP protocol on port 7765.

Quick start:

  Draw a circle for 5 seconds at 30 kpps:
    python etherdream_sender.py --host 192.168.1.50 --pps 30000 circle --seconds 5

  Draw a square:
    python etherdream_sender.py --host 192.168.1.50 --pps 30000 --packet-points 300 square --seconds 10

  Stream an ILDA file at 30 FPS and loop it:
    python etherdream_sender.py --host 192.168.1.50 --pps 30000 --packet-points 300 ild show.ild --fps 30 --loop

  Stop and blank the DAC:
    python etherdream_sender.py --host 192.168.1.50 stop

Ether Dream is not a one-way protocol. Every command receives an ACK, and the
DAC can answer "full" when its buffer cannot accept the next point block. This
sender retries the same block on "full" so the image does not skip ahead.
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
    wait_until_precise,
)


ETHERDREAM_PORT = 7765
RESP_SIZE = 22
POINT_STRUCT = struct.Struct("<HhhHHHHHH")
ED_RING_CAPACITY = 8192
DEFAULT_PREFILL_POINTS = 2048


def clamp_i16(v) -> int:
    return max(-32768, min(32767, int(round(v))))


def clamp_u16(v) -> int:
    return max(0, min(65535, int(round(v))))


def apply_blanking(p: LaserPoint) -> LaserPoint:
    return p.with_blanking_applied()


def recv_exact(sock: socket.socket, n: int) -> bytes:
    data = bytearray()
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            raise ConnectionError("Socket closed")
        data.extend(chunk)
    return bytes(data)


def response_buffer_fullness(raw: bytes) -> int:
    """Extract Ether Dream status.buffer_fullness from a 22-byte response."""
    if len(raw) < RESP_SIZE:
        return 0
    return struct.unpack_from("<H", raw, 2 + 10)[0]


class EtherDreamSender:
    """Small Ether Dream client for deterministic test streaming."""

    def __init__(self, host: str, port: int = ETHERDREAM_PORT, timeout: float = 5.0, quiet: bool = False):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.quiet = quiet
        self.sock: Optional[socket.socket] = None

    def connect(self) -> None:
        if self.sock is not None:
            return
        last_error: Optional[BaseException] = None
        for attempt in range(4):
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(self.timeout)
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            try:
                sock.connect((self.host, self.port))
                self.sock = sock

                # Ether Dream sends an initial status response immediately on connect.
                try:
                    raw = recv_exact(sock, RESP_SIZE)
                    if not self.quiet:
                        print(f"Initial status: {raw.hex()}")
                except socket.timeout:
                    if not self.quiet:
                        print("No initial status received")
                return
            except (OSError, ConnectionError) as exc:
                last_error = exc
                self.sock = None
                sock.close()
                if attempt < 3:
                    time.sleep(0.25)

        raise ConnectionError(f"Could not connect to Ether Dream server: {last_error}")

    def close(self) -> None:
        if self.sock is not None:
            self.sock.close()
            self.sock = None

    def command(self, payload: bytes, name: str = ""):
        if self.sock is None:
            self.connect()
        assert self.sock is not None
        self.sock.sendall(payload)
        raw = recv_exact(self.sock, RESP_SIZE)
        resp = raw[0:1]
        cmd = raw[1:2]
        if name and not self.quiet:
            print(f"{name}: resp={resp!r} cmd={cmd!r} status={raw.hex()}")
        return resp, cmd, raw

    def ping(self) -> None:
        self.command(b"?", "PING")

    def prepare(self) -> None:
        resp, _, raw = self.command(b"p", "PREPARE")
        if resp != b"a":
            raise RuntimeError(f"PREPARE failed: {raw.hex()}")

    def begin(self, pps: int, low_water_mark: int = 0) -> None:
        payload = b"b" + struct.pack("<HI", int(low_water_mark), int(pps))
        resp, _, raw = self.command(payload, f"BEGIN {pps} pps")
        if resp != b"a":
            raise RuntimeError(f"BEGIN failed: {raw.hex()}")

    def stop(self) -> None:
        self.connect()
        try:
            self.command(b"s", "STOP")
        except Exception:
            # The firmware returns invalid if already idle; for a user-facing
            # stop command that is still a successful final state.
            pass

    def encode_point(self, p: LaserPoint):
        p = apply_blanking(p)
        return (
            0,  # control flags; rate-change queue is not used by this sender
            clamp_i16(p.x),
            clamp_i16(p.y),
            clamp_u16(p.r),
            clamp_u16(p.g),
            clamp_u16(p.b),
            clamp_u16(p.i),
            clamp_u16(p.u1),
            clamp_u16(p.u2),
        )

    def send_points_packet(self, points: Sequence[LaserPoint]):
        if not points:
            return b"a", b"d", b""
        payload = bytearray()
        payload += b"d"
        payload += struct.pack("<H", len(points))
        for p in points:
            payload += POINT_STRUCT.pack(*self.encode_point(p))
        return self.command(bytes(payload))

    def send_points_retry_full(self, points: Sequence[LaserPoint]) -> bytes:
        """Send one point block, retrying the same block if the DAC is full."""
        while True:
            resp, _, raw = self.send_points_packet(points)
            if resp == b"a":
                return raw
            if resp == b"F":
                time.sleep(0.002)
                continue
            raise RuntimeError(f"DATA failed: resp={resp!r} raw={raw.hex()}")

    def prefill(self, points: Sequence[LaserPoint], pps: int, seconds: float, points_per_packet: int) -> int:
        requested = int(pps * max(0.0, seconds))
        target = max(DEFAULT_PREFILL_POINTS, points_per_packet, requested)
        target = min(target, ED_RING_CAPACITY - points_per_packet - 1)
        sent = 0
        idx = 0
        n = len(points)
        while sent < target:
            batch = []
            for _ in range(min(points_per_packet, target - sent)):
                batch.append(points[idx])
                idx = (idx + 1) % n
            raw = self.send_points_retry_full(batch)
            sent += len(batch)
            if response_buffer_fullness(raw) >= target:
                break
        return sent

    def stream_points(
        self,
        points: Sequence[LaserPoint],
        pps: int,
        seconds: Optional[float] = None,
        loop: bool = True,
        points_per_packet: int = 300,
        prefill_seconds: float = 0.20,
        low_water_mark: int = 0,
        stop_on_exit: bool = True,
    ) -> None:
        if not points:
            raise ValueError("No points to stream")
        if pps <= 0:
            raise ValueError("pps must be > 0")

        points_per_packet = max(1, min(3000, int(points_per_packet)))

        self.connect()
        self.ping()
        self.stop()
        self.prepare()

        prefilled = self.prefill(points, pps, prefill_seconds, points_per_packet)
        if not self.quiet:
            print(f"Prefilled {prefilled} points")
        # Use a conservative low-water value for compatibility with clients and
        # firmware that honor it. The current ESP firmware also has its own
        # startup fill threshold, so this is just an extra hint.
        if low_water_mark <= 0:
            low_water_mark = min(1024, max(1, prefilled // 2))
        self.begin(pps, low_water_mark)

        packet_interval = points_per_packet / float(pps)
        start = time.perf_counter()
        next_send = start
        idx = prefilled % len(points)
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

                self.send_points_retry_full(batch)
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
                print(f"Streamed {total} points in {elapsed:.2f}s ({total / elapsed:.0f} pps sent after prefill)")


def stream_ilda_file(sender: EtherDreamSender, path, pps, fps=30.0, loop=False, scale=1.0, points_per_packet=300, prefill_seconds=0.20) -> None:
    frames = load_ilda(path)
    if not frames:
        raise RuntimeError(f"No supported ILDA frames found in {path}")

    # Flatten/resample frames into a point stream. Ether Dream is a continuous
    # point stream, so we keep sending frame-sized chunks at the requested FPS.
    stream = []
    frame_points = max(1, int(round(pps / fps))) if fps > 0 else 0
    for frame in frames:
        pts = normalize_ilda_points(frame.points, scale=scale)
        if frame_points > 0:
            pts = resample_points(pts, frame_points)
        stream.extend(pts)

    sender.stream_points(
        stream,
        pps=pps,
        seconds=None if loop else len(frames) / max(1.0, fps),
        loop=loop,
        points_per_packet=points_per_packet,
        prefill_seconds=prefill_seconds,
    )


def build_arg_parser():
    examples = """examples:
  python etherdream_sender.py --host 192.168.1.50 circle --seconds 5
  python etherdream_sender.py --host 192.168.1.50 --packet-points 300 square --seconds 10
  python etherdream_sender.py --host 192.168.1.50 --pps 30000 ild show.ild --fps 30 --loop
  python etherdream_sender.py --host 192.168.1.50 stop
"""
    p = argparse.ArgumentParser(
        description="Ether Dream sender with ILW-style test commands",
        epilog=examples,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--host", required=True, help="DAC IP address")
    p.add_argument("--port", type=int, default=ETHERDREAM_PORT, help=f"TCP port, default {ETHERDREAM_PORT}")
    p.add_argument("--pps", type=int, default=30000, help="Point output rate. Default: 30000")
    p.add_argument("--packet-points", type=int, default=300, help="Points per Ether Dream data command. Default: 300")
    p.add_argument("--prefill", type=float, default=0.20, help="Seconds of points to prefill before Begin. Default: 0.20")
    p.add_argument("--low-water-mark", type=int, default=0, help="Ether Dream Begin low water mark. Default: 0")
    p.add_argument("--timeout", type=float, default=5.0)
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

    sub.add_parser("stop", help="blank output and stop playback")
    return p


def main():
    args = build_arg_parser().parse_args()
    sender = EtherDreamSender(args.host, args.port, timeout=args.timeout, quiet=args.quiet)
    try:
        if args.cmd == "stop":
            sender.stop()
            return 0
        if args.cmd == "square":
            pts = make_square(args.size, args.points_per_side)
            sender.stream_points(pts, args.pps, args.seconds, True, args.packet_points, args.prefill, args.low_water_mark)
            return 0
        if args.cmd == "circle":
            pts = make_circle(args.radius, args.points)
            sender.stream_points(pts, args.pps, args.seconds, True, args.packet_points, args.prefill, args.low_water_mark)
            return 0
        if args.cmd == "sine":
            pts = make_sine(args.width, args.height, args.points, args.cycles)
            sender.stream_points(pts, args.pps, args.seconds, True, args.packet_points, args.prefill, args.low_water_mark)
            return 0
        if args.cmd == "ild":
            stream_ilda_file(sender, args.path, args.pps, args.fps, args.loop, args.scale, args.packet_points, args.prefill)
            return 0
        raise RuntimeError("Unknown command")
    finally:
        sender.close()


if __name__ == "__main__":
    raise SystemExit(main())
