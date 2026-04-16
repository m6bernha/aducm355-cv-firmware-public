"""Parse firmware UART output into structured CV sweep records.

The firmware emits:

    [BOOT] ...
    [INFO] cv_init OK, RTIA=121.40 ohm phase=0.12 deg
    [INFO] sweep 1/5 complete
    ...
    [INFO] Sweep complete: 2000 samples, 5 sweeps averaged
    [INFO] RTIA cal: 121.40 ohm, phase 0.12 deg
    Voltage_mV, Current_uA
    -300.0, 0.1234
    -299.5, 0.1198
    ...
    [INFO] END_DATA

This module extracts one or more CvSweep records from such a log.
Use from Python or as a CLI:

    python parse_uart_csv.py capture.log
"""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import List


RTIA_LINE = re.compile(
    r"RTIA cal:\s*([-\d.]+)\s*ohm(?:,\s*phase\s*([-\d.]+)\s*deg)?",
    re.IGNORECASE,
)
SUMMARY_LINE = re.compile(
    r"Sweep complete:\s*(\d+)\s*samples,\s*(\d+)\s*sweeps averaged",
    re.IGNORECASE,
)
HEADER_LINE = re.compile(r"Voltage_mV,\s*Current_uA", re.IGNORECASE)
END_LINE = re.compile(r"END_DATA", re.IGNORECASE)
SAMPLE_LINE = re.compile(r"^\s*([+-]?[\d.]+)\s*,\s*([+-]?[\d.]+)\s*$")


@dataclass
class CvSweep:
    rtia_ohm: float | None = None
    rtia_phase_deg: float | None = None
    sample_count_declared: int | None = None
    num_sweeps_averaged: int | None = None
    voltages_mv: List[float] = field(default_factory=list)
    currents_ua: List[float] = field(default_factory=list)

    def sample_count(self) -> int:
        return len(self.voltages_mv)

    def is_complete(self) -> bool:
        if self.sample_count_declared is None:
            return self.sample_count() > 0
        return self.sample_count() == self.sample_count_declared


def parse_log(text: str) -> List[CvSweep]:
    """Return every CV sweep found in a firmware log."""
    sweeps: List[CvSweep] = []
    current: CvSweep | None = None
    in_data = False

    for raw_line in text.splitlines():
        line = raw_line.strip()

        m = RTIA_LINE.search(line)
        if m:
            if current is None:
                current = CvSweep()
            current.rtia_ohm = float(m.group(1))
            if m.group(2) is not None:
                current.rtia_phase_deg = float(m.group(2))
            continue

        m = SUMMARY_LINE.search(line)
        if m:
            if current is None:
                current = CvSweep()
            current.sample_count_declared = int(m.group(1))
            current.num_sweeps_averaged = int(m.group(2))
            continue

        if HEADER_LINE.search(line):
            if current is None:
                current = CvSweep()
            in_data = True
            continue

        if END_LINE.search(line):
            if current is not None:
                sweeps.append(current)
                current = None
            in_data = False
            continue

        if in_data and current is not None:
            m = SAMPLE_LINE.match(line)
            if m:
                current.voltages_mv.append(float(m.group(1)))
                current.currents_ua.append(float(m.group(2)))

    if current is not None and current.sample_count() > 0:
        sweeps.append(current)

    return sweeps


def parse_file(path: Path) -> List[CvSweep]:
    return parse_log(path.read_text(encoding="utf-8", errors="replace"))


def main() -> None:
    ap = argparse.ArgumentParser(description="Parse firmware UART logs.")
    ap.add_argument("log", type=Path, help="Captured UART log file")
    args = ap.parse_args()

    sweeps = parse_file(args.log)
    if not sweeps:
        print("No CV sweeps found in log", file=sys.stderr)
        sys.exit(1)

    for i, s in enumerate(sweeps, 1):
        print(f"--- Sweep {i} ---")
        print(f"  RTIA:             {s.rtia_ohm} ohm (phase {s.rtia_phase_deg} deg)")
        print(f"  declared samples: {s.sample_count_declared}")
        print(f"  averaged sweeps:  {s.num_sweeps_averaged}")
        print(f"  samples read:     {s.sample_count()}")
        print(f"  complete:         {s.is_complete()}")
        if s.voltages_mv:
            print(f"  V range:          {min(s.voltages_mv):.1f} .. {max(s.voltages_mv):.1f} mV")
            print(f"  I range:          {min(s.currents_ua):.4f} .. {max(s.currents_ua):.4f} uA")


if __name__ == "__main__":
    main()
