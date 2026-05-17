#!/usr/bin/env python3
"""ILW-NET v1 / ILW-UDP v1 sender for ILDAWave DACs.

The sender is intentionally simple: it sends one CONFIG packet, then streams
POINTS packets over UDP. The DAC owns the real output timing, buffering,
underrun handling, and safety blanking.

Quick start:

  Draw a circle for 5 seconds at 30 kpps:
    python ilw_sender.py --host 192.168.1.50 --format full8 --pps 30000 circle --seconds 5

  Draw a square with conservative 64-point UDP packets:
    python ilw_sender.py --host 192.168.1.50 --format full8 --pps 30000 --packet-points 64 square --seconds 10

  Stream an ILDA file at 30 FPS and loop it:
    python ilw_sender.py --host 192.168.1.50 --format full8 --pps 30000 --packet-points 64 ild show.ild --fps 30 --loop

  Stop and blank the DAC:
    python ilw_sender.py --host 192.168.1.50 stop

Notes:
  - "full" is an alias for "full8", the compact full-channel format.
  - Use Ethernet for high point rates when possible.
  - Start with --packet-points 64. Increase only if your DAC/network path is stable.
"""

from __future__ import annotations

import argparse
import math
import socket
import struct
import time
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional, Sequence


ILW_MAGIC = 0x49
ILW_TYPE_CONFIG = 1
ILW_TYPE_POINTS = 2
ILW_TYPE_CONTROL = 3
ILW_VERSION_1 = 1

ILW_FMT_XY = 1
ILW_FMT_XYRGB16 = 2
ILW_FMT_XYRGBI16 = 3
ILW_FMT_FULL16 = 4
ILW_FMT_XYRGB8 = 5
ILW_FMT_XYRGBI8 = 6
ILW_FMT_FULL8 = 7

ILW_CFG_OUTPUT_ENABLE = 0x01
ILW_CFG_CLEAR_BUFFER = 0x02
ILW_CFG_APPLY_DEFAULTS = 0x04

ILW_CMD_STOP = 0
ILW_CMD_OUTPUT_ON = 1
ILW_CMD_OUTPUT_OFF = 2
ILW_CMD_CLEAR_BUFFER = 3
ILW_CMD_PARK = 4

DEFAULT_PORT = 45457
PACE_SPIN_WINDOW = 0.001

POINT_FORMAT_SIZES = {
    ILW_FMT_XY: 4,
    ILW_FMT_XYRGB16: 10,
    ILW_FMT_XYRGBI16: 12,
    ILW_FMT_FULL16: 16,
    ILW_FMT_XYRGB8: 7,
    ILW_FMT_XYRGBI8: 8,
    ILW_FMT_FULL8: 10,
}


@dataclass
class LaserPoint:
    """Internal 16-bit point representation used before ILW packet encoding."""

    x: int
    y: int
    r: int = 65535
    g: int = 65535
    b: int = 65535
    i: int = 65535
    u1: int = 0
    u2: int = 0
    blanked: bool = False

    def with_blanking_applied(self) -> "LaserPoint":
        if self.blanked:
            return LaserPoint(self.x, self.y, 0, 0, 0, 0, self.u1, self.u2, True)
        return self


def clamp_i16(v) -> int:
    return max(-32768, min(32767, int(round(v))))


def clamp_u16(v) -> int:
    return max(0, min(65535, int(round(v))))


def u16_to_u8(v) -> int:
    return max(0, min(255, int(round(clamp_u16(v) / 257))))


def rgb8_to_u16(v) -> int:
    return clamp_u16(v * 257)


def blank_point(x=0, y=0) -> LaserPoint:
    return LaserPoint(x=x, y=y, r=0, g=0, b=0, i=0, blanked=True)


def wait_until_precise(deadline: float) -> None:
    """Sleep most of the wait, then spin briefly for smoother packet pacing."""
    while True:
        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            return
        if remaining > PACE_SPIN_WINDOW:
            time.sleep(remaining - PACE_SPIN_WINDOW)
        else:
            while time.perf_counter() < deadline:
                pass
            return


class ILWSender:
    def __init__(self, host: str, port: int = DEFAULT_PORT, bind: Optional[str] = None):
        self.addr = (host, port)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        try:
            self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
        except OSError:
            pass
        if bind:
            self.sock.bind((bind, 0))
        self.format = ILW_FMT_FULL8
        self.pps = 30000

    def close(self) -> None:
        self.sock.close()

    def send_config(
        self,
        fmt=ILW_FMT_FULL8,
        pps=30000,
        output_enable=True,
        clear_buffer=True,
        apply_defaults=False,
    ) -> None:
        """Configure the DAC before streaming point packets."""
        if pps <= 0:
            raise ValueError("pps must be > 0")
        if fmt not in POINT_FORMAT_SIZES:
            raise ValueError(f"Unsupported ILW format: {fmt}")

        self.format = fmt
        self.pps = pps
        pps100 = max(1, min(65535, int(round(pps / 100))))
        flags = (
            (ILW_CFG_OUTPUT_ENABLE if output_enable else 0)
            | (ILW_CFG_CLEAR_BUFFER if clear_buffer else 0)
            | (ILW_CFG_APPLY_DEFAULTS if apply_defaults else 0)
        )
        packet = struct.pack("<BBBBBH", ILW_MAGIC, ILW_TYPE_CONFIG, ILW_VERSION_1, fmt, flags, pps100)
        self.sock.sendto(packet, self.addr)

    def send_control(self, command: int) -> None:
        self.sock.sendto(struct.pack("<BBB", ILW_MAGIC, ILW_TYPE_CONTROL, command), self.addr)

    def stop(self) -> None:
        self.send_control(ILW_CMD_STOP)

    def output_on(self) -> None:
        self.send_control(ILW_CMD_OUTPUT_ON)

    def output_off(self) -> None:
        self.send_control(ILW_CMD_OUTPUT_OFF)

    def clear_buffer(self) -> None:
        self.send_control(ILW_CMD_CLEAR_BUFFER)

    def recommended_points_per_packet(self) -> int:
        # Conservative packet sizes keep UDP bursts short and leave headroom for
        # the ESP-to-RP flow-control path. Larger packets can work, but increase
        # latency and make whole-packet drops more visible if the DAC is busy.
        return {
            ILW_FMT_XY: 220,
            ILW_FMT_XYRGB16: 80,
            ILW_FMT_XYRGBI16: 80,
            ILW_FMT_FULL16: 64,
            ILW_FMT_XYRGB8: 96,
            ILW_FMT_XYRGBI8: 80,
            ILW_FMT_FULL8: 64,
        }.get(self.format, 64)

    def encode_point(self, p: LaserPoint) -> bytes:
        """Encode one internal point into the active ILW wire format."""
        p = p.with_blanking_applied()
        x = clamp_i16(p.x)
        y = clamp_i16(p.y)
        r = clamp_u16(p.r)
        g = clamp_u16(p.g)
        b = clamp_u16(p.b)
        i = clamp_u16(p.i)
        u1 = clamp_u16(p.u1)
        u2 = clamp_u16(p.u2)

        if self.format == ILW_FMT_XY:
            return struct.pack("<hh", x, y)
        if self.format == ILW_FMT_XYRGB16:
            return struct.pack("<hhHHH", x, y, r, g, b)
        if self.format == ILW_FMT_XYRGBI16:
            return struct.pack("<hhHHHH", x, y, r, g, b, i)
        if self.format == ILW_FMT_FULL16:
            return struct.pack("<hhHHHHHH", x, y, r, g, b, i, u1, u2)

        r8, g8, b8, i8, u18, u28 = map(u16_to_u8, (r, g, b, i, u1, u2))
        if self.format == ILW_FMT_XYRGB8:
            return struct.pack("<hhBBB", x, y, r8, g8, b8)
        if self.format == ILW_FMT_XYRGBI8:
            return struct.pack("<hhBBBB", x, y, r8, g8, b8, i8)
        if self.format == ILW_FMT_FULL8:
            return struct.pack("<hhBBBBBB", x, y, r8, g8, b8, i8, u18, u28)
        raise ValueError(f"Unsupported ILW format: {self.format}")

    def send_points_packet(self, points: Sequence[LaserPoint]) -> None:
        """Send one POINTS datagram. ILW v1 allows at most 255 points."""
        if not points:
            return
        if len(points) > 255:
            raise ValueError("POINTS packet supports max 255 points")
        payload = b"".join(self.encode_point(p) for p in points)
        header = struct.pack("<BBB", ILW_MAGIC, ILW_TYPE_POINTS, len(points))
        self.sock.sendto(header + payload, self.addr)

    def stream_points(
        self,
        points: Sequence[LaserPoint],
        pps: int,
        seconds: Optional[float] = None,
        loop=True,
        points_per_packet: Optional[int] = None,
        config=True,
        fmt: Optional[int] = None,
        stop_on_exit=True,
    ) -> None:
        """Stream a point list at a fixed average point rate."""
        if not points:
            raise ValueError("No points to stream")
        if fmt is not None:
            self.format = fmt
        if config:
            self.send_config(fmt=self.format, pps=pps, output_enable=True, clear_buffer=True, apply_defaults=True)

        chunk = max(1, min(255, points_per_packet or self.recommended_points_per_packet()))
        packet_interval = chunk / pps
        start = time.perf_counter()
        next_send = start
        idx = 0
        n = len(points)

        try:
            while True:
                if seconds is not None and time.perf_counter() - start >= seconds:
                    break

                batch = []
                for _ in range(chunk):
                    if idx >= n:
                        if not loop:
                            break
                        idx = 0
                    batch.append(points[idx])
                    idx += 1
                if not batch:
                    break

                self.send_points_packet(batch)
                next_send += packet_interval
                delay = next_send - time.perf_counter()
                if delay > 0:
                    wait_until_precise(next_send)
                elif delay < -0.25:
                    next_send = time.perf_counter()
        finally:
            if stop_on_exit:
                self.stop()


def interpolate_line(x0, y0, x1, y1, steps, color=(65535, 65535, 65535), blank=False) -> List[LaserPoint]:
    pts = []
    steps = max(1, steps)
    for k in range(steps):
        t = k / steps
        x = clamp_i16(x0 + (x1 - x0) * t)
        y = clamp_i16(y0 + (y1 - y0) * t)
        r, g, b = color
        pts.append(blank_point(x, y) if blank else LaserPoint(x, y, r, g, b, 65535))
    return pts


def make_square(size=22000, points_per_side=80) -> List[LaserPoint]:
    s = clamp_i16(size)
    corners = [(-s, -s), (s, -s), (s, s), (-s, s), (-s, -s)]
    colors = [(65535, 0, 0), (0, 65535, 0), (0, 0, 65535), (65535, 65535, 0)]
    pts = interpolate_line(0, 0, -s, -s, 20, blank=True)
    for idx in range(4):
        pts += interpolate_line(*corners[idx], *corners[idx + 1], points_per_side, colors[idx])
    pts += interpolate_line(-s, -s, 0, 0, 20, blank=True)
    return pts


def make_circle(radius=22000, points=360) -> List[LaserPoint]:
    pts = [blank_point(radius, 0)]
    for k in range(points):
        a = 2 * math.pi * k / points
        x = clamp_i16(radius * math.cos(a))
        y = clamp_i16(radius * math.sin(a))
        r = clamp_u16((math.sin(a) * 0.5 + 0.5) * 65535)
        g = clamp_u16((math.sin(a + 2.09439510239) * 0.5 + 0.5) * 65535)
        b = clamp_u16((math.sin(a + 4.18879020479) * 0.5 + 0.5) * 65535)
        pts.append(LaserPoint(x, y, r, g, b, 65535))
    pts.append(blank_point(radius, 0))
    return pts


def make_sine(width=26000, height=16000, points=400, cycles=2.0) -> List[LaserPoint]:
    pts = [blank_point(-width, 0)]
    for k in range(points):
        t = k / max(1, points - 1)
        x = clamp_i16(-width + 2 * width * t)
        y = clamp_i16(height * math.sin(2 * math.pi * cycles * t))
        pts.append(LaserPoint(x, y, 0, 65535, 65535, 65535))
    pts.append(blank_point(width, 0))
    return pts


DEFAULT_PALETTE_256 = [(i, i, i) for i in range(256)]


@dataclass
class ILDAFrame:
    name: str
    company: str
    points: List[LaserPoint]


def parse_ilda_header(data: bytes, offset: int):
    if offset + 32 > len(data) or data[offset : offset + 4] != b"ILDA":
        return None
    return {
        "format": data[offset + 7],
        "name": data[offset + 8 : offset + 16].rstrip(b"\x00 ").decode("ascii", errors="replace"),
        "company": data[offset + 16 : offset + 24].rstrip(b"\x00 ").decode("ascii", errors="replace"),
        "count": struct.unpack(">H", data[offset + 24 : offset + 26])[0],
        "data_offset": offset + 32,
    }


def load_ilda(path) -> List[ILDAFrame]:
    """Load common ILDA sections: palette, 2D/3D indexed, and 2D/3D true-color."""
    data = Path(path).read_bytes()
    frames = []
    palette = DEFAULT_PALETTE_256.copy()
    offset = 0

    while offset + 32 <= len(data):
        hdr = parse_ilda_header(data, offset)
        if hdr is None:
            nxt = data.find(b"ILDA", offset + 1)
            if nxt < 0:
                break
            offset = nxt
            continue

        fmt = hdr["format"]
        count = hdr["count"]
        pos = hdr["data_offset"]
        if count == 0:
            break

        pts = []
        if fmt == 0:
            size = 8
            end = pos + count * size
            if end > len(data):
                break
            for j in range(count):
                rec = data[pos + j * size : pos + (j + 1) * size]
                x, y, _ = struct.unpack(">hhh", rec[0:6])
                status = rec[6]
                ci = rec[7]
                blank = bool(status & 0x40)
                r, g, b = palette[ci % len(palette)]
                pts.append(blank_point(x, y) if blank else LaserPoint(x, y, rgb8_to_u16(r), rgb8_to_u16(g), rgb8_to_u16(b), 65535))
            frames.append(ILDAFrame(hdr["name"], hdr["company"], pts))
            offset = end
        elif fmt == 1:
            size = 6
            end = pos + count * size
            if end > len(data):
                break
            for j in range(count):
                rec = data[pos + j * size : pos + (j + 1) * size]
                x, y = struct.unpack(">hh", rec[0:4])
                status = rec[4]
                ci = rec[5]
                blank = bool(status & 0x40)
                r, g, b = palette[ci % len(palette)]
                pts.append(blank_point(x, y) if blank else LaserPoint(x, y, rgb8_to_u16(r), rgb8_to_u16(g), rgb8_to_u16(b), 65535))
            frames.append(ILDAFrame(hdr["name"], hdr["company"], pts))
            offset = end
        elif fmt == 2:
            size = 3
            end = pos + count * size
            if end > len(data):
                break
            pal = palette.copy()
            for j in range(min(count, 256)):
                pal[j] = tuple(data[pos + j * 3 : pos + j * 3 + 3])
            palette = pal
            offset = end
        elif fmt == 4:
            size = 10
            end = pos + count * size
            if end > len(data):
                break
            for j in range(count):
                rec = data[pos + j * size : pos + (j + 1) * size]
                x, y, _ = struct.unpack(">hhh", rec[0:6])
                status = rec[6]
                b = rec[7]
                g = rec[8]
                r = rec[9]
                blank = bool(status & 0x40)
                pts.append(blank_point(x, y) if blank else LaserPoint(x, y, rgb8_to_u16(r), rgb8_to_u16(g), rgb8_to_u16(b), 65535))
            frames.append(ILDAFrame(hdr["name"], hdr["company"], pts))
            offset = end
        elif fmt == 5:
            size = 8
            end = pos + count * size
            if end > len(data):
                break
            for j in range(count):
                rec = data[pos + j * size : pos + (j + 1) * size]
                x, y = struct.unpack(">hh", rec[0:4])
                status = rec[4]
                b = rec[5]
                g = rec[6]
                r = rec[7]
                blank = bool(status & 0x40)
                pts.append(blank_point(x, y) if blank else LaserPoint(x, y, rgb8_to_u16(r), rgb8_to_u16(g), rgb8_to_u16(b), 65535))
            frames.append(ILDAFrame(hdr["name"], hdr["company"], pts))
            offset = end
        else:
            print(f"Unsupported ILDA section format {fmt} at offset {offset}; stopping parse.")
            break
    return frames


def normalize_ilda_points(points, scale=1.0) -> List[LaserPoint]:
    return [
        LaserPoint(clamp_i16(p.x * scale), clamp_i16(p.y * scale), p.r, p.g, p.b, p.i, p.u1, p.u2, p.blanked)
        for p in points
    ]


def resample_points(points: Sequence[LaserPoint], target_count: int) -> List[LaserPoint]:
    """Resample one ILDA frame so every video frame contributes pps/fps points."""
    if not points or target_count <= 0:
        return []
    if len(points) == target_count:
        return list(points)
    if len(points) == 1:
        return [points[0]] * target_count

    out = []
    step = len(points) / target_count
    for i in range(target_count):
        out.append(points[min(len(points) - 1, int(i * step))])
    return out


def stream_ilda_file(sender, path, pps, fps=30.0, loop=False, fmt=ILW_FMT_FULL8, scale=1.0, points_per_packet=None) -> None:
    """Stream an ILDA file as repeated frames at the requested FPS."""
    frames = load_ilda(path)
    if not frames:
        raise RuntimeError(f"No supported ILDA frames found in {path}")

    sender.format = fmt
    sender.send_config(fmt=fmt, pps=pps, output_enable=True, clear_buffer=True, apply_defaults=True)
    frame_points = max(1, int(round(pps / fps))) if fps > 0 else 0
    idx = 0

    try:
        while True:
            pts = normalize_ilda_points(frames[idx].points, scale=scale)
            if frame_points > 0:
                pts = resample_points(pts, frame_points)
            sender.stream_points(pts, pps=pps, loop=False, points_per_packet=points_per_packet, config=False, fmt=fmt, stop_on_exit=False)
            idx += 1
            if idx >= len(frames):
                if not loop:
                    break
                idx = 0
    finally:
        sender.stop()


def parse_format(s):
    n = s.lower().replace("-", "").replace("_", "")
    aliases = {
        "xy": ILW_FMT_XY,
        "xyrgb16": ILW_FMT_XYRGB16,
        "rgb16": ILW_FMT_XYRGB16,
        "xyrgb": ILW_FMT_XYRGB16,
        "rgb": ILW_FMT_XYRGB16,
        "xyrgbi16": ILW_FMT_XYRGBI16,
        "rgbi16": ILW_FMT_XYRGBI16,
        "xyrgbi": ILW_FMT_XYRGBI16,
        "rgbi": ILW_FMT_XYRGBI16,
        "full16": ILW_FMT_FULL16,
        "x16": ILW_FMT_FULL16,
        "xyrgb8": ILW_FMT_XYRGB8,
        "rgb8": ILW_FMT_XYRGB8,
        "xyrgbi8": ILW_FMT_XYRGBI8,
        "rgbi8": ILW_FMT_XYRGBI8,
        "full8": ILW_FMT_FULL8,
        "compact": ILW_FMT_FULL8,
        "full": ILW_FMT_FULL8,
    }
    if n in aliases:
        return aliases[n]
    raise argparse.ArgumentTypeError("format must be one of: xy, xyrgb8, xyrgb16, xyrgbi8, xyrgbi16, full8, full16")


def build_arg_parser():
    examples = """examples:
  python ilw_sender.py --host 192.168.1.50 circle --seconds 5
  python ilw_sender.py --host 192.168.1.50 --packet-points 64 square --seconds 10
  python ilw_sender.py --host 192.168.1.50 --pps 30000 ild show.ild --fps 30 --loop
  python ilw_sender.py --host 192.168.1.50 stop
"""
    p = argparse.ArgumentParser(
        description="ILW-NET v1 / ILW-UDP v1 sender",
        epilog=examples,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--host", required=True, help="DAC IP address")
    p.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"UDP port, default {DEFAULT_PORT}")
    p.add_argument(
        "--format",
        type=parse_format,
        default=ILW_FMT_FULL8,
        help="xy, xyrgb8, xyrgb16, xyrgbi8, xyrgbi16, full8, full16. Default: full8",
    )
    p.add_argument("--pps", type=int, default=30000, help="Point output rate requested in CONFIG. Default: 30000")
    p.add_argument("--packet-points", type=int, default=None, help="Points per UDP packet. Default is format-specific; 64 is conservative.")
    p.add_argument("--bind", default=None, help="Optional local source IP to bind before sending")

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

    sub.add_parser("stop", help="blank output, clear DAC buffer, disable output")
    sub.add_parser("on", help="enable output after a valid CONFIG")
    sub.add_parser("off", help="blank and disable output")
    sub.add_parser("clear", help="clear DAC point buffer")
    sub.add_parser("park", help="park/blank if supported by firmware")
    return p


def main():
    args = build_arg_parser().parse_args()
    sender = ILWSender(args.host, args.port, bind=args.bind)
    sender.format = args.format

    try:
        if args.cmd == "stop":
            sender.stop()
            return 0
        if args.cmd == "on":
            sender.output_on()
            return 0
        if args.cmd == "off":
            sender.output_off()
            return 0
        if args.cmd == "clear":
            sender.clear_buffer()
            return 0
        if args.cmd == "park":
            sender.send_control(ILW_CMD_PARK)
            return 0
        if args.cmd == "square":
            pts = make_square(args.size, args.points_per_side)
            sender.stream_points(pts, pps=args.pps, seconds=args.seconds, loop=True, points_per_packet=args.packet_points, config=True, fmt=args.format)
            return 0
        if args.cmd == "circle":
            pts = make_circle(args.radius, args.points)
            sender.stream_points(pts, pps=args.pps, seconds=args.seconds, loop=True, points_per_packet=args.packet_points, config=True, fmt=args.format)
            return 0
        if args.cmd == "sine":
            pts = make_sine(args.width, args.height, args.points, args.cycles)
            sender.stream_points(pts, pps=args.pps, seconds=args.seconds, loop=True, points_per_packet=args.packet_points, config=True, fmt=args.format)
            return 0
        if args.cmd == "ild":
            stream_ilda_file(sender, args.path, pps=args.pps, fps=args.fps, loop=args.loop, fmt=args.format, scale=args.scale, points_per_packet=args.packet_points)
            return 0
        raise RuntimeError("Unknown command")
    finally:
        sender.close()


if __name__ == "__main__":
    raise SystemExit(main())
