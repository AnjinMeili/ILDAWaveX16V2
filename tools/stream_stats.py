#!/usr/bin/env python3
"""Sample /api/rp/status and print per-interval stream throughput.

Shows, for each interval: whether output is running, the RP2354 rate and
buffer fill, points/s arriving per protocol, and RP underrun samples/s.
Underruns are samples the RP blanked because its buffer was empty.

  Sample a running stream every 2 s, 10 times:
    python3 tools/stream_stats.py --host 192.168.1.173

  Wait up to 5 minutes for an Ether Dream stream to start, then sample:
    python3 tools/stream_stats.py --host 192.168.1.173 --wait etherdream

Read-only: it never starts or stops output.
"""

from __future__ import annotations

import argparse
import json
import time
import urllib.request

PROTOCOLS = ("etherdream", "idn", "ilw", "sd_ilda")


def get_status(host: str) -> dict:
    for _ in range(5):
        try:
            with urllib.request.urlopen(f"http://{host}/api/rp/status", timeout=3) as r:
                return json.load(r)
        except OSError:
            time.sleep(1)
    raise SystemExit(f"status unreachable at {host}")


def samples(status: dict, proto: str) -> int:
    return status.get(f"stream_{proto}_samples", 0)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="ildawavex16v2.local")
    ap.add_argument("--interval", type=float, default=2.0, help="seconds per sample")
    ap.add_argument("--count", type=int, default=10, help="number of samples")
    ap.add_argument("--wait", choices=PROTOCOLS, help="wait for this protocol's points to start arriving")
    ap.add_argument("--wait-timeout", type=float, default=300.0)
    args = ap.parse_args()

    if args.wait:
        base = samples(get_status(args.host), args.wait)
        deadline = time.time() + args.wait_timeout
        while True:
            s = get_status(args.host)
            if s["remote_running"] and samples(s, args.wait) > base:
                break
            if time.time() > deadline:
                raise SystemExit(f"no {args.wait} stream within {args.wait_timeout:.0f} s")
            time.sleep(1)
        time.sleep(3)  # skip the prebuffer and start-up transient

    prev, prev_t = get_status(args.host), time.time()
    for _ in range(args.count):
        time.sleep(args.interval)
        cur, t = get_status(args.host), time.time()
        dt = t - prev_t
        rates = " ".join(
            f"{p}/s={(samples(cur, p) - samples(prev, p)) / dt:6.0f}"
            for p in PROTOCOLS
            if samples(cur, p) != samples(prev, p)
        ) or "no points arriving"
        underruns = (cur["remote_underruns"] - prev["remote_underruns"]) / dt
        print(
            f'{time.strftime("%H:%M:%S")} run={int(cur["remote_running"])} '
            f'rate={cur["remote_sample_rate"]} fill={cur["remote_buffer_fill_samples"]:5d} '
            f"{rates} underruns/s={underruns:6.0f} "
            f'dropped+{cur["remote_stream_dropped_packets"] - prev["remote_stream_dropped_packets"]}'
        )
        prev, prev_t = cur, t


if __name__ == "__main__":
    main()
