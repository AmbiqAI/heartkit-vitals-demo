# Design record: real-time streaming pipeline rework

Status: proposed, pending owner approval (2026-08-31)
Scope: heartkit-vitals-demo firmware data plane + surrounding code structure
Inputs: two independent design passes (data plane; code structure), plus
hardware validation of the deployed build on an Apollo510B EVB.

## 1. Problem, as measured

The Tileio dashboard shows gaps in the ECG trace during completely normal
streaming - no stall, no USB error. Hardware run 2026-08-31 confirmed the
symptom persists after the host-side playout-clock fix.

### 1.1 Root cause (verified in source)

The ECG TX taps are written **only** from inside the segmentation branch, in
206-sample blocks:

- `src/main.cc:950-952` transfers/pushes `ECG_SEG_VALID_LEN` samples.
- `ECG_SEG_VALID_LEN = 256 - 2*25 = 206` (`src/constants.h:429-431`), which at
  `ECG_TARGET_RATE = 100` Hz is **2.06 s of signal** (the segmentation window
  is 256 samples, 2.56 s).
- That branch is an `else if` (`src/main.cc:929`) firing roughly once per 2 s.

`send_ecg_signals()` then runs every ~100 ms loop tick and pops at most 40
samples (`src/main.cc:675`).

**Steady-state ECG emission is therefore 5 packets x 40 samples inside ~500 ms,
then ~1.5 s of nothing.** The host receives 2000 ms of signal in a 500 ms
window, every 2 s.

The host retains at most 1500 ms (500 ms playout delay, discard beyond 1500 ms
stale). So **>= 500 ms of every 2 s block is discarded (~25% of all ECG
samples) and a gap renders every 2 s, in normal operation.** This is
independent of USB BUSY handling and of the host-side fix.

PPG does not do this: `PpgProcessTask` tees samples continuously
(`src/main.cc:1053,1059`), so it emits ~10-13 samples per 100 ms tick. That is
exactly why the tester saw ECG "chunkier than PPG".

### 1.2 The packet budget is already spent, on the wrong stream

| Stream | Today | Payload | Note |
|---|---|---|---|
| ECG signal | 2.5 pkt/s | 240 B | bursty: 5 packets per 2 s |
| PPG signal | 10 pkt/s | ~60-78 B | |
| CPU signal | 10 pkt/s | **14 B** | 1 sample/packet (`main.cc:1201-1207`) |
| Metrics | ~2 pkt/s | 12-44 B | |
| Total | ~24.5 pkt/s | | ~6.3 kB/s on the wire |

CPU utilisation burns 40% of the packet budget to carry 14 bytes at a time,
while the waveform that visibly gaps gets 10%. **The fix is a redistribution at
zero extra bandwidth.**

## 2. Blocking defect found independently by both design passes

`src/ringbuffer.c` **cannot represent a full buffer**. `head`/`tail` are both in
`[0, size-1]`, so `ringbuffer_len()` maxes at `size-1` and `ringbuffer_space()`
never returns 0. Therefore:

- `ringbuffer_push()`'s overflow guard (`ringbuffer.c:31`) is unreachable; a
  short write can never be reported. Same for `ringbuffer_transfer()`
  (`ringbuffer.c:104`) and `ringbuffer_fill()`.
- `sensor.c`'s `g_ecg_drop_count` / `g_ppg_drop_count` only increment on
  `pushed != sample_cnt` (`sensor.c:328-330,352-354,371-373`), so they **can
  never increment**.
- On overflow `head` laps `tail` and `len` collapses to 0: the buffer silently
  reports empty and **every buffered sample is lost**, with no counter.

Reachable today: `SENSOR_BUF_LEN = 256` (`constants.h:130`) is 1.28 s of ECG at
200 Hz; any `EcgProcessTask` stall past that wipes the sensor ring silently.

**Consequence for our evidence: the `ecg(push=5616 drop=0)` captured over SWO is
structurally guaranteed to read 0 and is not evidence of zero loss.** Any
earlier reasoning resting on those counters must be re-derived.

This must be fixed before any drop policy is meaningful.

## 3. Core design decisions

### 3.1 Two quantities, not one

- **D = fixed pipeline delay** (sensor to render). Dominated by AI windowing.
  Invisible on a scrolling waveform.
- **J = arrival jitter.** What the host's playout buffer absorbs and what the
  staleness rule punishes.

Today's code minimises D at the cost of unbounded J. **That is backwards for a
live monitor.** We deliberately trade D for a hard bound on J.

Firmware contracts:

```
TIO_JITTER_BUDGET_MS = 250   /* worst-case inter-packet spacing at the host */
TIO_MAX_EMIT_RATE    = 1.00x realtime   /* NEVER exceed. No catch-up, ever. */
```

250 ms is 50% of the host's 500 ms playout delay (2x margin) and 17% of the
1500 ms staleness threshold.

### 3.2 No catch-up, ever (the load-bearing argument)

After a stall of duration T the firmware holds T seconds of extra signal. Three
options:

1. **Flood at >1x (today).** Host queue grows by T, exceeds 1500 ms, discards -
   and discards the *newest* data in arrival order. Guaranteed loss plus a gap.
2. **Catch up at 1.1-1.25x.** Burning T seconds of backlog necessarily adds T
   seconds to the host's playout buffer *regardless of the catch-up rate*.
   Slower catch-up spreads the same +T over longer. The host ends permanently T
   closer to its staleness threshold. **There is no rate at which this works.**
3. **Discard T seconds of stale signal at the source, resume at exactly 1x.**
   Latency returns to nominal immediately; one honest gap of exactly T renders.

For a live monitor only (3) is correct. This is the single most important
decision in the rework.

### 3.3 Drop policy

> Every buffer in the signal path is freshness-preserving: on overflow discard
> the **oldest** data, count the discarded samples, and propagate the count to
> the wire as a sequence discontinuity.

Never drop-newest. Never block a producer. Never hoard. Primary enforcement is
a trim-to-high-water at the TX ring on each pump tick; sensor rings and
transport queues get drop-oldest as a safety net.

Loss signalling **reuses TimedSignal v2's per-slot sequence** (nsx-tileio#6) -
no competing discontinuity flag. Requires one clarification filed on that
issue: **sequence must count samples, not packets**, so the host can compute an
exact gap width from a sequence delta.

### 3.4 Cadence

| Slot | Samples/pkt | Interval | Rate | Change |
|---|---|---|---|---|
| ECG signal | 10 | 100 ms | 10 pkt/s | up from 2.5 |
| PPG signal | 10 | 100 ms (+33 ms phase) | 10 pkt/s | unchanged |
| CPU signal | 5 | 500 ms (+66 ms phase) | 2 pkt/s | **down from 10** |
| Metrics | 1 | >= 1000 ms capped | ~2 pkt/s | rate-capped |

Total ~24 pkt/s, ~6.1 kB/s - **the same as today**. A 72-byte payload in a
256-byte frame is 72% padding; at 6 kB/s on a full-speed bulk link that is
irrelevant and is the correct trade. Do not "optimise" it back.

### 3.5 Buffer sizing, derived rather than picked

`H` (trim high-water) `= structural_block + samples_per_packet + jitter_margin`;
capacity `D = H + structural_block`.

| Ring | Structural | H | Depth | Backlog cap | Was |
|---|---|---|---|---|---|
| ECG TX (x3) | 200 | 250 | 512 | 2.5 s | 500 |
| PPG TX (x2) | 13 | 63 | 128 | 0.63 s | 500 |
| CPU TX (x3) | 1 | 10 | 16 | 1.6 s | 500 |

The "5 s hoard" disappears: ECG caps at 2.5 s of which 2.0 s is structural, so
**max non-structural hoard = 500 ms**; PPG = 630 ms. Both below the host's
1500 ms threshold by construction. Net **~12.8 kB SRAM freed** (plus ~10 kB
more available by right-sizing the metrics buffers - separate follow-up).

### 3.6 Transport topology

Two bounded drop-oldest queues (depth 8 each), one drain task. Today's single
shared queue (`TIO_TX_QUEUE_DEPTH`) couples the transports. Since #56 a packet
held for USB retry backs the queue up rather than dropping what is behind it;
BLE keeps draining once occupancy reaches `TIO_TX_USB_HOLD_WATERMARK`, with the
released packets counted as USB loss (see Verification). Separate queues remain
the way to decouple the two transports fully.

USB BUSY handling simplifies: bounded <=15 ms wait, then drop that packet only
and continue. **Delete the latch and probe machinery** - it was engineered for
large rare packets and is the wrong shape for small frequent ones. Keep the
`nsx_usb_vendor_write_available()` pre-check (`tio_usb.c:369-371`) untouched; it
is the only thing preventing a 5 s blocking timeout in `nsx_usb_vendor_send()`.

### 3.7 Not-connected behaviour

Gate at the tee, not the enqueue: when no sink is attached, discard-in-place
instead of pushing to TX rings, and skip packing/CRC/enqueue entirely. Metrics
and inference keep running. Expose the predicate as `tio_stream_enabled()` so
issue #8 (configurable telemetry) extends the same seam rather than inventing a
second one. UIO stays alive in both directions regardless.

## 4. Observability (prerequisite for trusting any of the above)

- **Flag polarity is currently backwards**: the cheap 1 Hz counters are gated by
  `EN_APP_DEBUG_LOGS` (default 0, off) while the expensive ~30 lines/s
  per-iteration prints are gated by `EN_APP_TIMING_LOGS` (default **1**, on).
  Split into always-compiled cheap counters vs `EN_APP_TRACE` (default 0).
- **SWO corruption is a data race, not interleaving**: `am_util_stdio` formats
  into a single file-static `g_prfbuf`, and `vsnprintf` routes through the *same*
  buffer - so "format privately, emit atomically" does not work on this SDK.
  Format and emit must both sit inside one lock. Use a **mutex, not a critical
  section**: a ~200-char line is ~2 ms of ITM writes and masking interrupts that
  long would break the AS7058 bounded-INT window.
- Machine-parseable line format `HKV|<uptime_ms>|<seq>|<subsystem>|<k=v>...`
  with a report sequence number, so a missing SWO line is distinguishable from a
  firmware stall. Integers only with fixed-point suffixes (the current
  float-splitting idiom is expensive and loses the sign on negatives).
- `TaskStatus_t xTaskDetails[10]` vs 9 live tasks - one task away from silently
  reporting zero CPU per task. Raise to 16 and count the overflow.

## 5. Merged implementation sequence

Reconciles both design passes. Each step is independently buildable, flashable,
reviewable.

| # | Step | Behaviour change? |
|---|---|---|
| 0 | Land the issue #4 PR (`4-usb-busy-handling`) unmodified | already scoped |
| 1 | Host test harness (`tests/`, plain CMake, no framework) + `ringbuffer` full-wrap fix + `push_overwrite()` | correctness only |
| 2 | Observability: log serialisation, always-on counters, flag polarity, line format | yes (output) |
| 3 | Latency-budget constants + **rate-matched emission** - THE FIX | **yes - gaps disappear; ECG delay grows ~2 s** |
| 4 | Ring resize per 3.5 + drop-oldest everywhere | yes (bounded backlog, SRAM freed) |
| 5 | Transport extraction into `tio_stream.{c,h}` + split queues + simplified BUSY (subsumes issue #5's extraction bullet) | yes (USB stall no longer costs BLE) |
| 6 | Not-connected gating (also serves issue #8) | yes (CPU drop when idle) |
| 7 | Cadence hygiene: CPU 10->2 pkt/s, metrics capped, phases staggered | minor |
| 8 | Remaining structure work: pipeline TUs, dissolve `store.{h,c}`, split `constants.h`, static asserts | pure moves |
| 9 | TimedSignal v2 adoption - **blocked on nsx-tileio#6 + clock semantics (6.2)** | yes (needs host compat gate) |
| 10 | Optional, measured: AS7058 `fifo_threshold` 64->32 (halves sensor batching to ~65 ms) | minor |

Step 3 alone should eliminate the reported gaps. Everything after is hardening.

Do **not** rebase the #4 branch across a file split; land it first. The
uncommitted `EN_APP_DEBUG_LOGS 0->1` edit must not be committed - step 2
supersedes it.

## 6. Decisions the owner must make

### 6.1 ECG display latency grows to ~2.5-5 s (product call)

This is the deliberate D-for-J trade. The delay is dominated by the denoise and
segmentation windows, which are inherent to the models. The only real lever is
halving the stride (200 -> 100), which halves block delay at **2x inference
cost**. Needs an explicit decision before step 3, because someone will see a
multi-second lag between touching the electrode and the trace moving and call it
a regression.

### 6.2 Host staleness semantics (blocking risk for step 9)

If the host's staleness test were ever absolute (`host_now - source_ms`), then
with D = 2.5-5 s **everything would be discarded**. The test must be on
playout-queue residency, never on absolute clock difference. Must be settled on
nsx-tileio#6 before TimedSignal v2 is implemented.

### 6.3 Visible-loss transition

Fixing the ring buffer makes previously-hidden loss **visible**. It is not new
loss, but the dashboard may look worse before it looks better. Land it with
real counters in place so the loss is quantified - and not in the week of a
customer demo.

## 7. Acceptance telemetry

Steady state, per second, per slot:

- `pkt_rate[slot]` = 10 +/- 1 for ECG and PPG signal slots; no burst > 2 packets
  within 20 ms for the same slot.
- `pkt_gap_max_ms[slot]` <= `TIO_JITTER_BUDGET_MS` with the host draining, and
  **still the single most important number in the exercise.** A host read gap
  no longer turns into loss: while queue occupancy stays below
  `TIO_TX_USB_HOLD_WATERMARK` the affected packets arrive late rather than
  short, and delivery resumes within one drain poll of the host draining again.
  Past the watermark USB loses packets to keep the queue bounded, counted in
  `tiousb drop`, so the criterion there is that the loss is counted and stops
  when the backlog clears.
- `trim[slot]` = 0 in steady state (non-zero is a bug, not a policy).
- `txlen_max[slot]` <= H; `qdepth_max[usb|ble]` <= 2 steady, <= 8 ever.
- `as7058 isr interval` min ~= max; a max > 3x min is IRQ starvation.
- `ecg/ppg drop` = 0, and now trustworthy after step 1.
- Enqueue `fail` = 0 with a sink attached; no packing at all with none.

Test matrix: 60 s USB baseline; 60 s BLE; USB+BLE concurrent; induced 30 s USB
stall; 20x disconnect/reconnect; 10 min with no host (counters flat, CPU delta
recorded); 8 h soak; 10 min dashboard visual check.

The induced stall is the criterion that moved. BLE must lose no packet the
queue accepted: BLE delivery is delayed while USB holds a packet, but the delay
is bounded by `TIO_TX_USB_HOLD_WATERMARK` rather than by the stall length, and
BLE is back at full rate once the watermark starts releasing. On resume the USB
packet rate returns to 10/s, not 40/s.
