"""Compare a firmware sweep against a golden reference sweep.

Validation criteria (matches the targets in rewrite_plan.md):

  * Per-sample relative error on mean current, < 5 percent
  * Pearson cross-correlation of the sweep waveforms, > 0.95
  * Peak current error, < 10 percent
  * Peak position error, < 2 percent of sweep length

Usage:

    python compare_sweeps.py golden.csv candidate.log
    python compare_sweeps.py golden.csv candidate.csv --format csv

Both inputs can be either firmware UART logs (default) or two-column
CSVs in the same Voltage_mV, Current_uA format the firmware emits.
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import List, Tuple

from parse_uart_csv import CvSweep, parse_file


@dataclass
class CompareResult:
    passed: bool
    sample_count_match: bool
    mean_rel_err: float
    correlation: float
    peak_current_rel_err: float
    peak_position_frac_err: float
    notes: List[str]

    def summary(self) -> str:
        status = "PASS" if self.passed else "FAIL"
        lines = [
            f"Comparison: {status}",
            f"  sample count match:        {self.sample_count_match}",
            f"  mean relative error:       {self.mean_rel_err * 100:.2f} %",
            f"  Pearson correlation:       {self.correlation:.4f}",
            f"  peak current rel error:    {self.peak_current_rel_err * 100:.2f} %",
            f"  peak position frac error:  {self.peak_position_frac_err * 100:.2f} %",
        ]
        lines += [f"  note: {n}" for n in self.notes]
        return "\n".join(lines)


def load_csv(path: Path) -> CvSweep:
    sweep = CvSweep()
    with path.open("r", encoding="utf-8", errors="replace") as fh:
        reader = csv.reader(fh)
        for row in reader:
            if len(row) != 2:
                continue
            try:
                v = float(row[0].strip())
                i = float(row[1].strip())
            except ValueError:
                continue
            sweep.voltages_mv.append(v)
            sweep.currents_ua.append(i)
    return sweep


def load_firmware(path: Path) -> CvSweep:
    sweeps = parse_file(path)
    if not sweeps:
        raise SystemExit(f"{path}: no CV sweeps found")
    return sweeps[-1]


def pearson(a: List[float], b: List[float]) -> float:
    n = len(a)
    if n == 0 or len(b) != n:
        return 0.0
    mean_a = sum(a) / n
    mean_b = sum(b) / n
    num = sum((a[i] - mean_a) * (b[i] - mean_b) for i in range(n))
    da = math.sqrt(sum((a[i] - mean_a) ** 2 for i in range(n)))
    db = math.sqrt(sum((b[i] - mean_b) ** 2 for i in range(n)))
    if da == 0 or db == 0:
        return 0.0
    return num / (da * db)


def mean_rel_err(reference: List[float], candidate: List[float]) -> float:
    n = min(len(reference), len(candidate))
    if n == 0:
        return float("inf")
    scale = max(abs(x) for x in reference) or 1.0
    return sum(abs(candidate[i] - reference[i]) for i in range(n)) / (n * scale)


def compare(golden: CvSweep, candidate: CvSweep) -> CompareResult:
    notes: List[str] = []

    n_g = golden.sample_count()
    n_c = candidate.sample_count()
    sample_count_match = n_g == n_c
    if not sample_count_match:
        notes.append(f"sample count mismatch: golden={n_g} candidate={n_c}")

    n = min(n_g, n_c)
    ref = golden.currents_ua[:n]
    cand = candidate.currents_ua[:n]

    mre = mean_rel_err(ref, cand)
    corr = pearson(ref, cand)

    ref_peak_idx = max(range(n), key=lambda i: ref[i]) if n else 0
    cand_peak_idx = max(range(n), key=lambda i: cand[i]) if n else 0
    ref_peak = ref[ref_peak_idx] if n else 0.0
    cand_peak = cand[cand_peak_idx] if n else 0.0
    peak_rel_err = abs(cand_peak - ref_peak) / abs(ref_peak) if ref_peak else float("inf")
    peak_pos_frac_err = abs(cand_peak_idx - ref_peak_idx) / n if n else 1.0

    passed = (
        sample_count_match
        and mre < 0.05
        and corr > 0.95
        and peak_rel_err < 0.10
        and peak_pos_frac_err < 0.02
    )

    return CompareResult(
        passed=passed,
        sample_count_match=sample_count_match,
        mean_rel_err=mre,
        correlation=corr,
        peak_current_rel_err=peak_rel_err,
        peak_position_frac_err=peak_pos_frac_err,
        notes=notes,
    )


def main() -> None:
    ap = argparse.ArgumentParser(description="Compare a firmware sweep against golden.")
    ap.add_argument("golden", type=Path)
    ap.add_argument("candidate", type=Path)
    ap.add_argument("--format", choices=["log", "csv"], default="log",
                    help="How to interpret the candidate file (default: log)")
    ap.add_argument("--golden-format", choices=["log", "csv"], default="csv",
                    help="How to interpret the golden file (default: csv)")
    args = ap.parse_args()

    golden = load_csv(args.golden) if args.golden_format == "csv" else load_firmware(args.golden)
    candidate = load_csv(args.candidate) if args.format == "csv" else load_firmware(args.candidate)

    result = compare(golden, candidate)
    print(result.summary())
    sys.exit(0 if result.passed else 2)


if __name__ == "__main__":
    main()
