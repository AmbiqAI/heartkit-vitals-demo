# Stripped normal-sleep hardware check

Issue #68. AP510B probe1160002954, JS110004204 on the owner's MCU supply rail.
September 9, 2026. Image: `hkv_sleep_minimal`, SHA256
`db7ef93a528c5298edf0f60e5111b9932d08ef153574782a7e3822ad2aa5e98e`.
No firmware changes between these two attempts.

## Result

| Measurement, capture02 seconds30-60 | Value |
| --- | ---: |
| Mean rail voltage | 1.810406 V |
| Mean current | 0.836722 mA |
| Mean instantaneous power | 1.514824 mW |
| Settled interval | 30 s |
| Five-second mean-power range | 1.514741-1.514966 mW |

The complete packed gate signal has one rising edge at24.75103s and no falling
edge. All gate samples during the settled interval are high; no gate deglitching
was used. Analog intervals are aligned by JLS timestamps using `read_interval`;
no nonfinite samples were found. Use a full gate read followed by slicing:
an offset packed-GPI read produced spurious leading zero bits in this pyjls
environment, while repeated full reads agree on the single edge.

This is below the earlier full-memory3.67060mW capture, but still about2.02 times
the datasheet750uW Sleep1 reference. Memory, reset and debug conditions differ,
so this comparison does not isolate a memory-only saving. It does not establish
the optimized sleep floor or validate the demo's battery-life projection.

## Attempt01: rejected, debug domain remained powered

NSX flashed and verified the binary. A brief SWO viewer session observed the
countdown, then detached before its end. The pre-WFI gate never rose. Settled
current was about3.05mA, not a sleep measurement. A subsequent SWO session
reported `PERIPHERAL_STILL_ON status=26 stage=1`, identifying the debug domain.
The helper deliberately stayed awake in its failure-report loop.

Evidence: `sleep-minimal01.jls`, `sleep-minimal-startup01.log`,
`sleep-minimal-after01.log`. Earlier peripheral and memory readback checks had
passed to reach the countdown. No guard was removed to get attempt02 to pass.

## Attempt02: SWPOI reset, no startup viewer

Started JS110 recording, then used:

```sh
hpx target reset \
  --board apollo510b_evb --jlink-serial 1160002954 --kind swpoi
```

No SWO or debug-memory attachment during recording. After recording completed,
used HPX's `attached_session` to read memory without resetting or halting.
The pre-WFI snapshot and the sticky sleep-entry bit provide independent
evidence beyond the absence of prints. Debug attachment itself can affect power;
those reads are outside the reported analog interval.

| Field | Pre-WFI snapshot | Interpretation |
| --- | --- | --- |
| stage | 2 | HAL pre-WFI hook reached |
| CPUPWRCTRL | 0x00000000 | Ambiq clock-gated sleep selection |
| SCB SCR | 0x00000000 | Normal sleep, not deep sleep |
| DEVPWREN / DEVPWRSTATUS | 0 / 0 | Listed peripheral domains off, including debug |
| MEMPWREN / MEMPWRSTATUS | 0x19 / 0x59 | Small TCM configuration, MRAM banks and cache powered |
| MEMRETCFG | 0x2 | TCM retained |
| SSRAMPWREN / SSRAMPWRST | 0 / 0 | Shared SRAM off |
| SSRAMRETCFG | 0x7 | No shared-SRAM retention |
| VRSTATUS | 0x30 | Buck active before WFI; not an in-sleep regulator reading |
| MRAMCRYPTOPWRCTRL | 0x200 | Saved for follow-up; no proof of NVM standby |
| CLKGEN OCTRL / CLOCKENSTAT | 0x80 / 0x44000000 | Saved for follow-up clock audit |
| SYSPWRSTATUS | 0x3 | Sticky sleep flags cleared before entry |

After recording, live SYSPWRSTATUS was0x20000003: CORESLEEP set. The saved
`g_sleep_wake_status` remained0. Live DEVPWRSTATUS was0x04000000 after debugger
attachment, unlike the pre-WFI snapshot. Do not treat that debug-on state as the
state during the capture.

Register decoding source: vendored Apollo510 generic CMSIS header under
`modules/nsx-ambiq-sdk/modules/nsx-ambiqsuite/sdk/CMSIS/AmbiqMicro/Include/`.
Memory locations were verified against this ELF's symbols:
`g_sleep_snapshot=0x200043d0`, `g_sleep_wake_status=0x200043cc`.

## Next and board state

The stripped image remains installed, not restored to the demo. A debug-memory
attachment occurred after recording; perform the same clean reset without a
startup viewer before another quiet-power capture. Keep raw `sleep-minimal02.jls`.

Remaining gap: verify in-sleep clock/regulator and MRAM-standby policy against
Sleep1 conditions before further changes. The successful attempt changed both
reset kind and viewer attachment, so do not attribute recovery exclusively to
one. No production power constants or dashboard values changed.
