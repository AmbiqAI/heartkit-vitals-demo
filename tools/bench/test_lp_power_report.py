#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
import tempfile
import unittest
from pathlib import Path
import numpy as np
from pyjls import Writer, SignalType, DataType
from lp_power_report import summarize, deglitch


class ReportTest(unittest.TestCase):
    def test_gate_deglitch_limit(self):
        self.assertEqual(deglitch([(10, 90), (98, 310), (400, 408), (500, 800)], 10),
                         [(10, 310), (500, 800)])
        self.assertEqual(deglitch([(10, 90), (101, 310)], 10), [(10, 90), (101, 310)])

    def fixture(self, path, windows=15, corrupt=False, offset=0):
        fs = 1000
        n = 65000
        gate = np.zeros(n, dtype=np.uint8)
        gate[1000:1100] = 1
        for i in range(windows):
            start = 2100 + 4000 * i
            gate[start:start + 3000] = 1
        with Writer(str(path)) as writer:
            writer.source_def(1, name="fixture", vendor="test", model="test")
            for sid, name, units, value in ((1, "current", "A", 0.003),
                                           (2, "voltage", "V", 1.8),
                                           (3, "power", "W", 0.0054)):
                writer.signal_def(sid, 1, signal_type=SignalType.FSR,
                                  data_type=DataType.F32, sample_rate=fs,
                                  name=name, units=units)
                data = np.full(n, value, dtype=np.float32)
                if corrupt and sid == 3:
                    data[2500] = np.nan
                writer.fsr(sid, 0, data)
                writer.utc(sid, offset, 100 * (1 << 30))
                writer.utc(sid, offset + 64000, 164 * (1 << 30))
            writer.signal_def(4, 1, signal_type=SignalType.FSR,
                              data_type=DataType.U1, sample_rate=fs, name="gpi[0]")
            writer.fsr(4, 0, np.packbits(gate, bitorder="little"))
            writer.utc(4, 0, 100 * (1 << 30))
            writer.utc(4, 64000, 164 * (1 << 30))

    def test_full_capture(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "test.jls"
            self.fixture(path, offset=20)
            rows = summarize(path)
            self.assertEqual(len(rows), 15)
            self.assertEqual(rows[-1]["phase"], "light_sleep")
            for row in rows:
                self.assertAlmostEqual(row["power_mW"], 5.4, places=5)
                self.assertAlmostEqual(row["energy_mJ"], 16.2, places=4)

    def test_incomplete_capture(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "test.jls"
            self.fixture(path, windows=14)
            with self.assertRaisesRegex(ValueError, "Expected 15 windows"):
                summarize(path)

    def test_missing_power_samples(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "test.jls"
            self.fixture(path, corrupt=True)
            with self.assertRaisesRegex(ValueError, "nonfinite"):
                summarize(path)


if __name__ == "__main__":
    unittest.main()
