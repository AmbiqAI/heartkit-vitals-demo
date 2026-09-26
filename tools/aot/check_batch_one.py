# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026, Ambiq
"""Boundary checks for the fixed shipped-model batch specialization."""
import struct
import unittest

from specialize_arrhythmia import ROOT, specialize


class BatchOneTests(unittest.TestCase):
    def test_only_declared_signature_fields_change(self):
        original = (ROOT / "assets/arrhythmia.tflite").read_bytes()
        patched, evidence = specialize(original)
        expected = bytearray(original)
        for change in evidence["changes"]:
            self.assertEqual(struct.unpack_from("<i", original, change["offset"])[0], -1)
            struct.pack_into("<i", expected, change["offset"], 1)
        self.assertEqual(bytes(expected), patched)
        self.assertEqual(len(evidence["changes"]), 48)
        self.assertEqual(len(evidence["cases"]), 8)
        self.assertTrue(all(case["bit_exact"] for case in evidence["cases"]))

    def test_rejects_changed_source_before_interpretation(self):
        original = bytearray((ROOT / "assets/arrhythmia.tflite").read_bytes())
        original[-1] ^= 1
        with self.assertRaisesRegex(ValueError, "unexpected source model"):
            specialize(bytes(original))


if __name__ == "__main__":
    unittest.main()
