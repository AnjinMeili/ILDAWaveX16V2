# Galvo safety review and tuning plan

Review of the ILDAWaveX16 V2 firmware for behaviour that can damage the
attached scanners, with hardware measurements taken on 2026-10-23 against a
DT50K galvo set rated 50 kpps. Companion material: `docs/research/hardware.md`
(schematic, DAC80508, galvos, ISP-DB25, other DACs), `docs/research/protocols.md`
(Ether Dream, IDN, ILDA file format) and `docs/research/ilda-tuning.md` (ILDA
Test Pattern, tuning practice, point optimisation). Test tooling:
`tools/hwtest.py` (board behaviour tests) and `tools/ild_inspect.py` (step
analysis of .ild files).

## 1. What the output stage does with a DAC code

From the schematic (`docs/research/hardware.md` section 1.3): the DAC80508
X and Y outputs (0 to 5 V) pass through an LM324 level shifter computing
`X+ = 2 x Vdac - 5 V`, then a unity inverter for `X-`. So:

| DAC code | signed sample | X+ / X- | differential | meaning |
|---|---|---|---|---|
| 0x0000 | -32768 | -5 V / +5 V | -10 V | full-scale corner |
| 0x8000 | 0 | 0 V / 0 V | 0 V | scanner centre |
| 0xFFFF | +32767 | +5 V / -5 V | +10 V | opposite corner |

Colour channels are single-ended 0 to 5 V; code 0 is laser off. There is no
analog filtering or slew limiting on X/Y: the LM324's 0.5 V/us slew is the
only thing softening a step, and a full-scale step cannot complete inside one
sample at 50 kpps.

The DT50K's rating is angle-dependent: vendors quote about 45 kpps at the
8 degree ILDA test-pattern angle and 50 kpps only at 5 to 6 degrees, falling
to about 35 kpps at 15 to 20 degrees. Galvo damage is thermal, from sustained
fast, wide or square-wave drive. A DC hold is harmless to the galvo.

## 2. Findings

Status key: **fixed+verified** (on the board today), **planned**. RP 0.2.0
was flashed in the evening once the USB cable was flipped; the whole suite
was then re-run against it (raw tables in local logs, not in the repository).

### F1. STOP, CLEAR and boot parked the scanners in a corner. Fixed+verified.
`dac_write_all_zero()` wrote raw 0x0000 to all eight channels. On X/Y that is
-10 V differential, a full-scale corner, and the scanners stayed there,
coils at full deflection, until the next START, which then stepped them to
centre and from centre to the first point. Every Ether Dream disconnect,
IDN close or timeout, ILW stop, SD playback end and web Stop did this.
Fix (RP 0.2.0): a parked state with X/Y at centre and laser channels at 0 V,
written at boot, STOP, CLEAR and BUFFER. Boot now reaches the parked state
about 1 ms after the DAC soft reset instead of 20 ms. Verified: the RP
console logged 58 STOP/START pairs during the suite, every PARKED report at
`commanded_xy=(0,0)` with the laser channels blanked, and the boot line
`output=PARKED`.

### F2. Stops and starts were single-sample jumps. Fixed+verified.
STOP jumped from the last point to the corner; START jumped from centre to
the first point. RP 0.2.0 replaces both with blanked 10 ms linear moves
(16 to 1024 samples at the current rate): STOP ramps from the last
commanded X/Y to centre before parking; START peeks at the first sample in
the ring and ramps from centre to it before the stream plays. The stream's
own first point is unchanged, so no client sees retimed data; the stream
starts 10 ms later. The first build of this had a defect found on the board:
the ramp re-armed after it finished (no real sample had been popped yet, so
the arming condition still held) and the stream never played; the ring sat
at its prebuffer with zero underruns. A once-per-START latch fixed it;
start-to-running stayed 53 to 129 ms over 20 cycles with the ramp in place.

### F3. Any client could run the DAC at up to 200 kpps. Fixed+verified.
The web API accepted 200000, Ether Dream Begin and update accepted 100000,
IDN derived up to 100000 from chunk timing, ILW up to 200000; the RP ran
whatever it was told. Measured before the fix: requests for 60k, 100k and
200k all applied; an Ether Dream Begin at 100k ran at 100k.
Fix (ESP 0.1.1+, flashed): a scanner rate limit saved in NVS
(`POST /api/rp/rate_limit?value=`, default 30000, status field
`rate_limit`). Every RATE goes through `rp_manager_set_rate_applied()`, which
clamps and reports the applied rate; Ether Dream status and broadcast carry
the applied rate and the limit; the web page disables rate buttons above the
limit. Measured after: requests for 60k, 100k, 200k apply 50k (the limit set
for the DT50K); Begin at 100k runs at 50k and the client is told 50k; the
limit survives a reboot and the RP's stale 50k rate was pulled down at boot.
RP 0.2.0 adds its own copy (`LIMIT max_rate=`, saved in flash, STATUS field
`max_rate`), which the ESP keeps in step, so the limit holds even if the ESP
side is bypassed. Verified: after the RP flash it came up with its default
30000 while the ESP held 50000; the ESP only synced at its own boot. Setting
the limit through the API pushed `LIMIT max_rate=50000` (RP console) and the
RP kept it across its next reboot. The ESP now also re-syncs on every status
read when the two disagree (flashed; exercised only in the agreeing case).

### F4. A STOP with a full send queue left one stale packet on the RP. Fixed+verified.
`rp_manager_output_stop()` reset the queue and sent STOP at once; a packet
already on the SPI wire landed after the RP cleared its ring. Measured: 4 of
5 stops after 2 s of full-queue streaming left 256 samples on the RP, which
the next START would have played from the parked centre (a jump to the old
image). Fix (ESP): stop and clear paths wait for the in-flight transfer, as
the cancel path already did; 20/20 clean on both firmwares. RP 0.2.0 adds a
second guard: a packet whose reception straddled a STOP or CLEAR is
discarded (`stale_packets` in STATUS, `remote_stale_packets` in the status
JSON).

### F5. IDN and Ether Dream dropped samples silently. Fixed+verified.
When a protocol's ring was full the remainder of a chunk was discarded with
no counter. Measured: an IDN sender at twice its declared rate for 3 s lost
about 74,000 of 180,000 points with nothing in the status. Fix: per-protocol
`stream_*_dropped` counters in the status JSON. Verified: a 3 s flood at
twice the declared rate lost 73,536 points by the client's count and the
counter read 65,345; the 8,191 difference is the ESP ring's content at the
moment the session closed, discarded with the session rather than dropped.

### F6. Ether Dream clients were never told about underruns. Fixed+verified.
The real Ether Dream sets playback_flags bit 1 on underflow. The board held
position and blanked (better for the galvos than the real DAC's stop) but
never set the flag. Fix: the RP poll task sets the flag when the RP's underrun
count grows while playing; Prepare clears it. Verified in `ed-underrun-hold`.

### F7. STATUS reads over UART failed under load. Fixed+verified.
The reader gave the whole reply 200 ms from the command and read one byte per
call; when the ESP32 was busy the task was not scheduled for that long and
long STATUS lines arrived truncated (6 failures in 23 minutes of testing).
Fix: read whatever is buffered in one call and extend the deadline while
bytes keep arriving. 0 failures in the following hour of heavier testing.

### F8. IDN loss depends on packet rate. Measured; cause partly open.
Before the ESP reflash, 64-point IDN packets (469 packets/s at 30 kpps) lost
3.4 percent and 12 to 15 percent at 50 kpps; 120 and 180-point packets were
clean at 30 kpps. After the reflash the same sweep lost at most 0.2 percent
at every size and rate. The reboot and the UART-reader change happened
together, so the cause is not isolated; the sweep is in `hwtest.py` to
re-run. Senders should still prefer 120 to 180 points per packet.

### F9. A repeated IDN close opened a session. Fixed+verified.
An RT_CLOSE with no session open created one, started the feed task and
closed it again. Harmless but noisy; now ignored (acknowledged if asked).

### F10. ILDA files on the card contain step inputs. Planned (tuning).
`tools/ild_inspect.py` over the SD card copies: GRID.ILD has 20 lit steps of
72 percent of full scale, 1004.ILD one of 56 percent, ILDATEST.ILD lit
steps to 31 percent and blanked moves to 61 percent. These files rely on
the DAC or software to interpolate. The SD player already interpolates its
loop return over 64 samples; it does nothing inside a frame. See the plan.

### F11. ILW CONFIG rewrites RP flash. Planned.
Each ILW CONFIG compares the RP's buffer watermarks with ILW's constants and,
when they differ (they do on this board), sends BUFFER, which stops output
and writes RP flash. Not exercised this session for that reason.

### F12. The shutter output is never driven. Planned.
DB-25 pin 13 idles open through a pull-up; ESP GPIO40 (`ShutterL`) could
close it when parked. Interlock pins 4 and 17 are hard-wired on the board.

### F13. The step counters catch step inputs from any source. Verified.
RP 0.2.0 counts, per START, the largest lit X/Y step and the number of lit
and blanked steps of 1/8 full scale or more. During the suite they read
140 to 288 (0.2 to 0.4 percent, the circle's point spacing) for most
streams, and 20,915 to 23,737 with one to three lit big steps for others:
the test client restarts its circle at point zero after prefilling, a lit
jump of about 35 percent of full scale. A real client doing the same would
show up the same way in `remote_max_lit_step` and `remote_lit_big_steps`.

### F14. No colour/blanking delay control. Fixed+verified (2026-10-24).
The scanners lag the commanded position by 100 us or more and the colours do
not, so lit segments start and end early along the direction of travel. The
owner saw the fringes with Modulaser at 28.9 kpps and 81 fps with the colour
alignment known good. Fix: RP `SHIFT color_us=<-2000..2000>` (settings record
v3, applied as whole samples at the current rate, up to 128), ESP
`POST /api/rp/color_shift?value=` and `?save=1`, status fields
`remote_color_shift_us`, `remote_color_shift_samples` and
`remote_color_shift_saved`, and Apply, Save and -10/-1/+1/+10 us controls on
the Output Mapping card. Positive delays the colours behind X/Y.

Two defects in the first build were fixed before it was flashed. Changing the
shift reseeded the delay history from the last output sample, which is taken
after the mirror: while drawing with Mirror Y on, X/Y could jump to the
mirrored side for up to 128 samples, and a rate change mid-stream did the
same. The delay line now records the stream all the time (256-sample
history) and moves one sample per output sample toward a new value; a host
test across 0, +3, -3, +60, -60 us-equivalent changes kept every X/Y step
within two path steps (300 units against a 150-unit path). And each SHIFT
wrote RP flash, which pauses the RP while the laser draws; a SHIFT now saves
only while output is stopped, and `SHIFT save` is refused while running.

A Colour Shift test pattern (Test Stream) draws red, green and blue
columns, each with two short white lines drawn left to right: the upper
one ends at the column, the lower one starts there, and the beam runs
through the column blanked at line speed. Too little shift stops the upper
line short and carries the lower one across the column; too much does the
reverse; the right shift has both just touching. (Changed 2026-10-27 to
the owner's design; the first version blanked a short index gap on each
column, and a wrong shift staggered the gaps of neighbouring rows.) Verified on the board: the RP
settings survived the flash (with upstream's 2 MiB flash size), the owner
stepped the shift live and judged the pattern "works just like it should",
and saved 140 us (4 samples at 30 kpps).

### F15. Test patterns could jump lit and leave output running. Fixed+verified (2026-10-24).
Picking a pattern while one ran restarted the generator at the new
pattern's first point with no blanked move: the owner's run logged 2 lit
steps of 23,000 units (35 percent of full scale). And Stop Stream stopped the
generator but never released the output it had started, so the RP stayed
started, holding the last point blanked, and each restart stacked another
hold. Fix (ESP): a pattern change while running is a blanked move from the
last point to the new first point (512 units per sample, then 8 samples of
dwell); the test stream releases its own hold when it stops; and Stop Stream
now stops everything, like the top STOP, since the top START button holds
output too. Verified: a host test over all 36 pattern pairs kept lit steps at
the patterns' own (largest 2,893, the circle's seam); on the board, switching
Square, Colour Shift, Circle, Heart gave 0 lit big steps, and Square then
Stop Stream logged `output ref 1 -> 0, STOP ESP_OK` with output stopped.

### Owner's verification, same evening
With both 0.2.0 firmwares and Modulaser over IDN: test frames "dead exact
on"; the stream held at 28.9k, 40k, 45k and 50k (50k judged to be pushing
the DT50K and backed off to 40k; the board itself had no drops at any rate);
a rotating cube showed rounded corners, which is corner dwell in the client,
not the board; the owner's earlier Modulaser tuning no longer matched,
because it had compensated for the slow rate and lit padding fixed earlier
the same day.

| rate | arriving | RP buffer | underrun samples/s |
|---|---|---|---|
| 28,858 | 28,600 to 29,000 | 416 to 1,024 | 0 to 252 |
| 40,000 | 39,900 to 40,200 | 480 to 1,568 | 0 to 252 |
| 45,008 | 44,700 to 45,200 | 576 to 1,824 | 235 to 832 |
| 50,000 | 49,700 to 50,250 | 288 to 640 | 1,177 to 1,729 |

The underruns are Modulaser's send gaps against the IDN buffer, which only
holds what the 3,072-sample start prebuffer left; a configurable prebuffer
depth is the obvious follow-up for IDN and ILW.

## 3. Measurements

Board and Mac client on the same wired Ethernet LAN,
laser on a backstop. All figures from `/api/rp/status` after a forced STATUS
read, or from Ether Dream replies. The raw tables are in local logs, not in the repository.

### Rate limit

| request path | asked | applied before | applied after (limit 50000) |
|---|---|---|---|
| web set_rate | 60000 | 60000 | 50000 |
| web set_rate | 100000 | 100000 | 50000 |
| web set_rate | 200000 | 200000 | 50000 |
| Ether Dream Begin | 100000 | 100000 (client told 100000) | 50000 (client told 50000) |

### Start, stop and cancel (Ether Dream, 30 kpps, 3072-point prebuffer)

| test | runs | result |
|---|---|---|
| start after Begin | 20 | 53 to 129 ms, median 57 ms |
| stop to running=false and empty | 20 | 53 to 118 ms, median 56 ms |
| cancel before START, Ether Dream | 20 | 20/20 RP empty |
| cancel before START, IDN | 20 | 20/20 RP empty |
| stop with full queue, before fix | 5 | 4/5 left 256 samples |
| stop with full queue, after fix | 20 | 20/20 RP empty |

### Underrun (prefill 1024 points, then silence for 1 s)

Output stayed running, X/Y held, laser blanked; 26,112 to 26,208 blanked
samples in the second; no packet drops; after the fix the Ether Dream
underflow flag was set.

### Ether Dream link, one data command at a time, 30 kpps

| points per command | bytes | round trip median | p95 | max | sustainable |
|---|---|---|---|---|---|
| 100 | 1,811 | 8.9 ms | 11.3 ms | 85 ms | 11,200 pps |
| 300 | 5,411 | 14.0 ms | 17.0 ms | 83 ms | 21,400 pps |
| 600 | 10,811 | 20.6 ms | 76.2 ms | 92 ms | 29,100 pps |
| 1000 | 18,011 | 30.4 ms | 100.1 ms | 104 ms | 32,900 pps |

Round trip is about 7 ms plus 1.3 ms per kilobyte. Commands above the
11,520-byte TCP window show the retransmit stalls in their tails. A client
must pipeline (Modulaser does) to hold 30 kpps; with two 300-point commands
in flight the test client reached 29 to 30 kpps.

Re-measured after both firmwares were updated (RP 0.2.0, ESP 0.2.0): median
round trips 6.0 / 8.7 / 15.0 / 22.6 ms for 100 / 300 / 600 / 1000 points,
p95 within 1 to 3 ms of the median and no stalls above 28 ms. Which change
helped is not isolated; the UART-reader fix is the likely one, since STATUS
retries had been holding the client task's core.

### Throughput, 8 s circles, after the ESP fixes

| protocol | rate | radius | arriving/s | max underruns/s | lost on ESP |
|---|---|---|---|---|---|
| IDN | 30000 | 22000 | 30,112 | 0 | 0 |
| IDN | 40000 | 12000 | 39,993 | 762 | 0 |
| IDN | 50000 | 8000 | 50,035 | 1,350 | 640 (0.16%) |
| Ether Dream | 30000 | 22000 | 29,381 | 2,447 | 0 |
| Ether Dream | 40000 | 12000 | 30,163 (client-bound) | 15,261 | 0 |
| Ether Dream | 50000 | 8000 | 29,959 (client-bound) | 27,632 | 0 |

Baseline IDN figures the same day before the reflash: 24,381 / 32,143 /
45,322 arriving with 10,112 / 61,120 / 37,760 points lost on the ESP.
Ether Dream underruns at 40 and 50 kpps are the client falling short, not
the board; the per-second figure can exceed the rate because the status
cache updates once a second and the sampler's interval is measured
separately. Ether Dream reached_rp exceeds sent by the 3072-point prebuffer,
which the client does not count.

### IDN packet size (after reflash)

| rate | points/packet | packets/s | lost |
|---|---|---|---|
| 30000 | 64 | 469 | 0.0% (3.4% before reflash) |
| 30000 | 120 | 250 | 0.2% |
| 30000 | 180 | 167 | 0.1% |
| 50000 | 64 | 781 | 0.2% (11.6% before) |
| 50000 | 120 | 417 | 0.0% (15.0% before) |
| 50000 | 180 | 278 | 0.2% (4.8% before) |

## 4. Firmware changes

### RP2354 (0.2.0) - flashed and verified
- `output_engine.c`: PARKED state (centre, laser 0 V) replaces HARD_ZERO;
  10 ms blanked ramps on STOP (last X/Y to centre) and START (centre to the
  first sample, by peeking the ring); rate limit with `LIMIT max_rate=` and
  clamped `RATE` (reply `OK RATE <applied>`); per-stream step counters
  `max_lit_step`, `lit_big_steps`, `blank_big_steps` (steps of 8192 or more);
  core 1 signals idle so STOP never races a batch write.
- `dac80508.c`: `dac_write_park()`; boot sequence reset, 1 ms, gain, sync,
  park.
- `stream_rx.c` / `ring_buffer.c`: ring epoch; a packet received across a
  STOP or CLEAR is discarded and counted as `stale_packets`.
- `rp_settings.c`: record version 2 with `max_rate_hz` (default 30000);
  version 1 records are still read; version 3 adds `color_shift_us`, and
  versions 1 and 2 are still read.
- `output_engine.c` / `rp_uart.c` (2026-10-24): colour shift delay line
  (always-recording history, one-sample slew); `SHIFT color_us=` saves only
  while stopped, `SHIFT save`; STATUS `color_shift_us`,
  `color_shift_samples`, `color_shift_saved`.

Flashed with `picotool load -f -x build/ILDAWaveX16V2-RP2350.uf2` after
the cable flip; `hwtest.py all` passed 12 of 13 against it (the failure is
the test client stalling for a second during `ed-rate-change`). The RP USB
log was kept locally.

### ESP32-S3 (0.2.0) - flashed and verified
- `rp_manager.c`: NVS rate limit; `rp_manager_set_rate_applied()`; LIMIT push
  to RP 0.2.0; boot-time clamp of the RP's current rate; STOP/CLEAR wait for
  the in-flight packet; new STATUS fields parsed.
- `rp_uart.c`: chunked reads with a 100 ms inter-byte deadline.
- `etherdream_protocol.c`: applied rate in status; limit advertised as
  `max_point_rate`; underflow flag; ring-full drops counted.
- `idn_protocol.c`: hysteresis on the requested rate; drops counted; stray
  closes ignored.
- `protocol_stream_stats.c`: dropped counters.
- `web_server.c`: `/api/rp/rate_limit`; status fields `rate_limit`,
  `remote_max_rate`, `remote_requested_rate`, `remote_max_lit_step`,
  `remote_lit_big_steps`, `remote_blank_big_steps`, `stream_*_dropped`;
  rate buttons 10/20/30/40/50 kpps disabled above the limit; a scanner
  limit field.
- 2026-10-24: Scanner rating presets (12K to 50K) beside the limit field;
  `/api/rp/color_shift` (`?value=`, `?save=1`) with Apply, Save and step
  controls; `remote_color_shift_*` and `remote_stale_packets` in the status
  JSON; the Colour Shift test pattern; blanked moves between test patterns;
  Stop Stream stops everything.

## 5. Plan: standard ILDA tuning

Grounded in `docs/research/ilda-tuning.md`. Rule from the research that
shapes the split: **never re-time Ether Dream, IDN or ILW streams** (their
clients pace by point count and have already optimised); optimise only what
the ESP itself generates (SD files, test patterns). Per-sample transforms
(size, invert, offset, colour delay) are safe on any stream and belong on
the RP, after the ring, where timing is exact.

### Tier 1 - prerequisites (RP unless noted)
| feature | parameter | unit / range | default | notes |
|---|---|---|---|---|
| rate limit | `max_rate` | pps, 1000..200000 | 30000 | done, both chips |
| parked idle | - | - | centre, 0 V | done |
| soft start/stop | `soft_move_us` | us, 0..50000 | 10000 | done; expose later |
| ILDA Test Pattern generator | `test_pattern_rate` | 12000 or 30000, locked | 30000 | ESP; ILDATEST.ILD is on the card, but a generator removes the file dependency and pins the rate |
| stop reason / fault latch | `stopped_reason`, `estop_latched` | enum | - | ESP status; Ether Dream 'c' model |

### Tier 2 - projector settings page
| feature | parameter | unit / range | default | MCU |
|---|---|---|---|---|
| size | `size_x`, `size_y` | percent, 10..100 | 100 | RP (SD player has it today, ESP side) |
| invert / swap | `invert_x`, `invert_y`, `swap_xy` | bool | mirror as today | RP |
| offset | `offset_x`, `offset_y` | signed, -16384..16384 | 0 | RP |
| colour / blanking delay | `color_shift_us` | us, -2000..2000 | 0 | done: RP delay line after the ring; this board's set measured 140 us (4 points at 30K) |
| per-channel colour delay | `shift_r/g/b_us` | us | 0 | RP, after the above |
| colour mapping | `color_min/max`, `gamma`, `ttl` | V, 0..5; 1.0..2.6 | 0, 5, 1.0, off | RP |
| stationary-beam guard | `beam_guard_ms`, `beam_guard_fs` | ms, 0..100; percent | 20, 0.5 | RP; a lit beam that has not moved 0.5 percent of full scale in 20 ms is blanked; the amp's scan-fail remains the primary device |
| no-data watchdog | - | - | present (20 ms failsafe) | RP |
| speed derating table | angle to rate | per scanner | DT50K: 20 deg 35k, 15 deg 35k, 10 deg 40k, 8 deg 45k, 5 deg 50k | ESP: warn when size x rate exceeds the table |

### Tier 3 - point optimisation for ESP-originated frames only
| feature | parameter | default | notes |
|---|---|---|---|
| max lit step | `max_lit_step` | 1% of full scale | insert points along long lit segments (OpenLase: ceil(dist / max_step)) |
| max blanked step | `max_blank_step` | 5% | same for blanked moves |
| corner dwell | `dwell_points`, `dwell_angle` | 8 at 0 deg, 0 at 180 deg, threshold 30 deg | OpenLase cos-threshold rule |
| start / end repeats | `start_wait`, `end_wait` | 3 / 3 | |
| blanking lead-in/out | `blank_pre_us`, `blank_post_us` | 100 / 0 | Modulaser defaults |
| minimum frame points | `min_frame_points` | 200 | pad short frames to limit frame rate on a frame-mode source |

With these, GRID.ILD's 72 percent single-step lines become drawn lines, and
the `lit_big_steps` counter on the RP shows whether a source is still
sending step inputs.

### Tier 4
Horizontal blank zone, keystone, scanner-load
gauge (from the step counters and rate), shutter control on GPIO40 (close
when parked), ILW moved off the RP BUFFER command.

## 6. Open items
- IDN/ILW start prebuffer depth as a setting (latency vs smoothness).
- The ESP build that re-syncs the rate limit on every status read is
  flashed; the disagreeing case itself (RP rebooting with an older flash
  copy) was not reproduced after the flash, only the agreeing case.
- Fix the test client so `ed_feed` continues the pattern where the prefill
  stopped; the step counters flagged the restart.
- Confirm the DAC80508 variant (Z or M) from the chip marking; it decides
  what the outputs do between power-on and the first park write.
- Isolate F8: re-run `idn-packet-size` after long uptime and after heavy
  Ether Dream use to see whether loss returns.
- Decide the shipped default for the rate limit (30000 proposed) and
  whether Ether Dream should NAK rates above it (the real DAC does) rather
  than clamp (friendlier to operators). Clamping is implemented.
