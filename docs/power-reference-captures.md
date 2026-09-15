# Power reference captures

The shared reference values were selected from the September 15, 2026 AP510
HPX SRAM/MRAM campaign. Tracking: [#68](https://github.com/AmbiqAI/heartkit-vitals-demo/issues/68).

| Model | LP power (mW) | HP power (mW) | LP energy (µJ/inf) | HP energy (µJ/inf) |
| --- | ---: | ---: | ---: | ---: |
| Denoise FP32 | 6.577 | 22.099 | 102.804 | 134.892 |
| Segmentation | 5.393 | 17.614 | 306.438 | 386.205 |
| Arrhythmia | 6.265 | 21.112 | 51.161 | 66.520 |

HPX uses a dedicated transport-free image and GPIO-gated windows. All six
captures report valid results, complete inference counts, and OK terminals.
Integrated energy divided by completed count reproduces the table. Inputs
are synthetic; these runs do not assess model accuracy or a complete sensor
pipeline. The source record does not establish the physical supply rail's
downstream circuitry; these are adopted budgeting references, not certified
SoC-only measurements.

Scratch resides in shared SRAM and constants are read in place from MRAM.
The models match the repository's original flatbuffer hashes. The capture
uses CORE v7.35.0 and Arm GNU 14.3.1; adopting its power references does not
upgrade the demo's runtime or compiler. LP/HP clocks are 96/250 MHz.

Source records: `heartkit-sram-mram-review-2026-09-15.md`,
`heartkit-sram-mram-comparison-2026-09-15.csv`, and
`heartkit-sram-mram-evidence-2026-09-15.zip`.
HPX revision: `a9d73ee91976efb2b79aff3faf47ec29a67001c7`.
AOT revision: `bf0fd47d33077150b34251fa60eccbd3d26c850f`.
CORE revision: `cad3c8fa0cc2f7b13d6ff750bfc9744af0d621a6`.
