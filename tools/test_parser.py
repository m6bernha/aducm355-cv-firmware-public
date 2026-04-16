"""Unit tests for parse_uart_csv.parse_log.

Run with:
    python -m unittest test_parser.py
"""

import unittest

from parse_uart_csv import parse_log


SAMPLE_LOG = """\
[BOOT] aducm355-cv-firmware
[BOOT] MCU resource init OK
[INFO] LFOSC measured: 32752.4 Hz
[INFO] cv_init OK, RTIA=121.40 ohm phase=0.12 deg
[INFO] sweep 1/2 complete
[INFO] sweep 2/2 complete
[INFO] Sweep complete: 4 samples, 2 sweeps averaged
[INFO] RTIA cal: 121.40 ohm, phase 0.12 deg
Voltage_mV, Current_uA
-300.0, 0.1234
-100.0, 0.2500
+100.0, 0.3712
+300.0, 0.1100
[INFO] END_DATA
[INFO] idle
"""


class ParseTests(unittest.TestCase):
    def test_single_sweep(self):
        sweeps = parse_log(SAMPLE_LOG)
        self.assertEqual(len(sweeps), 1)
        s = sweeps[0]
        self.assertEqual(s.sample_count(), 4)
        self.assertEqual(s.sample_count_declared, 4)
        self.assertEqual(s.num_sweeps_averaged, 2)
        self.assertAlmostEqual(s.rtia_ohm, 121.40)
        self.assertAlmostEqual(s.rtia_phase_deg, 0.12)
        self.assertTrue(s.is_complete())
        self.assertEqual(s.voltages_mv, [-300.0, -100.0, 100.0, 300.0])
        self.assertAlmostEqual(max(s.currents_ua), 0.3712)

    def test_truncated_sweep(self):
        truncated = "\n".join(SAMPLE_LOG.splitlines()[:-3])  # drop END_DATA
        sweeps = parse_log(truncated)
        self.assertEqual(len(sweeps), 1)
        self.assertFalse(sweeps[0].is_complete())


if __name__ == "__main__":
    unittest.main()
