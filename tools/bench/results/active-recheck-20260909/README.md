# Running LP demo power comparison

Tracking: [issue #68](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/68).
Local AP510B hardware results, not a released change or battery-life guarantee.

## Results

JS110 on the owner's MCU supply rail. Live sensor input, LP, continuous Python
USB host draining, all other production settings unchanged. Each case streams
for 50 seconds; after 10 seconds settling, record 35 seconds and analyze seconds
5 through 30 of that recording. No debugger viewer during analog recording.

| Configuration | Mean power | Mean current | Five-second power means, min–max |
| --- | ---: | ---: | ---: |
| AI on, first run | 10.5127 mW | 5.8118 mA | 10.4979–10.5327 mW |
| All three AI modes off | 10.4408 mW | 5.7719 mA | 10.4338–10.4504 mW |
| AI on, repeat | 10.5221 mW | 5.8170 mA | 10.5080–10.5334 mW |

Measured voltage was approximately 1.809 V. Averaging the two AI-on runs gives
10.5174 mW, 0.0766 mW (0.73%) above AI-off. This is a difference between complete
running states, not isolated inference power or energy per inference.

## Interpretation

Use approximately **10.5 mW** as the measured running-demo MCU-rail average for
this setup. Approximately **10.44 mW** remains when the three AI model modes are
off. That baseline combines idle spinning, sensor servicing, remaining DSP,
USB streaming, normal reporting, and enabled peripheral/radio infrastructure.
This experiment does not separate those contributors.

`configUSE_TICKLESS_IDLE=0` in config/FreeRTOSConfig.h. The demo does not enter
the previously tested quiet sleep state between work. Turning off AI replaces
some useful compute with awake idle time, not with low-power sleep. The small
difference therefore must not be advertised as total inference power.

The sensor's own supply and LEDs are not isolated by this MCU-rail setup.
Live signal frames were received, but no physiological signal-quality or
sensor-supply power claim is made. BLE software remains initialized; it was
not connected or independently disabled for these captures. Its clocks/power
are not separately measured. Normal report/debug configuration also remains
enabled, even though the viewer was detached during analog recording.

The earlier 1.268 mW sleep helper used a different peripheral and memory policy.
Do not interpret their approximately 9.25 mW difference as a controlled
active-only increment. Likewise, do not replace the firmware's general-compute
constant with 10.44 mW: it includes a persistent system baseline, not just the
busy fraction of general compute. No power constants changed in this test.

## Validation and artifacts

- NSX programmed and verified the normal demo after current-path recovery.
- Each captured case echoed only the requested live/LP/AI state: input6,
  speed0, model modes2/2/2 or0/0/0.
- All three host runs completed with zero CRC errors and over 1,500 packets.
- Analog intervals were finite, positive, and timestamp-aligned across channels.
- Final SWO checks, after analog capture, show advancing sensor samples,
  no bus errors, missed interrupts, model errors or CPU-stat overflow.
- A final 15-second USB check delivered 505 packets with zero CRC errors.

Each case folder contains result.json, capture.jls, usb.log and record.log.
JLS and logs remain local/ignored; JSON preserves means, five-second blocks,
state and binary identity. Within-run spread is not instrument accuracy or
independent reproducibility across boards, wiring or physiological inputs.

The preliminary pre-reset.jls is invalid as a board-power measurement:
JS110 current range was off. Setting s/i/range/select=auto restored MCU access.
Final readback was128 (auto), and the path remains enabled.

Installed normal-demo SHA256:
`b00d8f79810f95b611fad18c52bffdd32cd8daed9f81e16efe66ba0016e85c9f`.
Board left in live input, LP, all three AI modes on; Python host/viewer stopped.

Reproduce a case with tools/bench/capture_demo_power.py, --ai on/off, a new
--name and --joulescope-serial for the connected fixture. It enables the
JS110 current path using auto range. It never flashes and refuses existing output directories. All checks
passed on three hardware cases; the four existing analog-report host tests
also pass. Before reusing the output to change an active-power assumption,
separate the quiet compute baseline from the demo's always-on overhead.
