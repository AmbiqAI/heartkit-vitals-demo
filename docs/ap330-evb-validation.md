# AP330 Plus EVB sensor validation

Tracking: #37, draft PR #85. Hardware acceptance is pending.

## Target and mapping

Apollo330 Plus EVB revision 2.0, target `apollo330mP_evb`.
Source: Apollo330 Plus EVB Quick Start Guide v2.0, QS-A330MP-2p0,
page 14, Figure 6 (J14/J15 mikroBUS).

| Signal | GPIO | Peripheral |
| --- | --- | --- |
| SCL | 25 | IOM2 |
| SDA | 26 | IOM2 |
| AS7058 IT1 / socket INT | 107 | GPIO rising-edge interrupt |

The app applies the BSP's IOM2 click pin configurations explicitly because
the generic BSP IOM pin helper omits that case. The command queue uses the
IOM2 ISR and NVIC interrupt. AP510/AP510B retain IOM1 and GPIO50 INT.

## FAE acceptance

1. Confirm EVB and Click revisions. With power off, use the intended click
   socket and board/module supply configuration. The guide identifies J11
   MBUS_IF_PWR pins 1-2 as the 3.3 V setting. Follow the matching module's
   power requirements; do not substitute raw GPIO jumpers.
2. Build and flash `apollo330mP_evb` with NSX. Custom J-Link support for
   `Apollo330P_510L` may be required with the configured board debug target.
3. Capture SWO and confirm `pin=107`, `async reads on IOM2`,
   `sensor_init OK`, and `sensor_start OK` with no initialization errors.
4. Run `tools/tileio_usb_test.py` for at least 60 seconds in LP/all-AI mode.
   Check CRC errors, ECG/PPG/CPU streams, and advancing model counters.
5. Check live sensor input and prerecorded input in TileIO; retain board
   revision, firmware hash, SWO and USB logs with the result.

R1.0 external-header testing is discontinued. It established flashing and
startup output, not sensor operation. No passing R2.0 hardware result is
claimed by the build checks.
