import unittest

from denoise_report import validate


def capture():
    lines = []
    for mode in ("lp", "hp"):
        for case in range(8):
            lines.append(f"HKV|parity|den mode={mode} ref=tflm case={case} rc=0 finite=1 "
                         "max_abs_nano=4291 cycles=100 ref_cycles=400 pass=1")
        lines.append(f"HKV|parity|den_summary mode={mode} ref=tflm pass=8/8 ref_init=0")
    return "\n".join(lines)


class ReportTests(unittest.TestCase):
    def test_complete_and_repeated(self):
        self.assertEqual(validate(capture()), 4291)
        self.assertEqual(validate(capture() + "\n" + capture()), 4291)

    def test_empty_or_incomplete(self):
        for text in ("", "\n".join(capture().splitlines()[1:]),
                     "\n".join(capture().splitlines()[:-1])):
            with self.assertRaises(ValueError):
                validate(text)

    def test_failed_evidence(self):
        for old, new in (("finite=1", "finite=0"), ("rc=0", "rc=1"),
                         ("pass=1", "pass=0"), ("ref_cycles=400", "ref_cycles=0"),
                         ("pass=8/8", "pass=7/8")):
            with self.assertRaises(ValueError):
                validate(capture().replace(old, new, 1))

    def test_conflicting_repetition(self):
        with self.assertRaises(ValueError):
            validate(capture() + "\n" + capture().replace("max_abs_nano=4291", "max_abs_nano=100"))


if __name__ == "__main__":
    unittest.main()
