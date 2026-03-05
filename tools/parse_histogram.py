#!/usr/bin/env python3
"""
Parse and dump histogram bin counts from SAT solver logs via unified_parser.

This script reads logs from a folder (supports seed* subdirectories) and dumps
CSV files with raw histogram bin counts provided by unified_parser for:
  - watchers_bins
  - variables_bins

We include finished and unknown tests (SAT/UNSAT/UNKNOWN).

Usage:
    python tools/parse_histogram.py <logs_folder> [output_dir]

Example:
        python tools/parse_histogram.py ../sat-isca26-data/base_128KB/profile_base_l1_4_1_l2_8_32/seed0/ results/

Outputs (in output_dir):
    - watchers_bins.csv                # aggregated counts across all tests
    - variables_bins.csv               # aggregated counts across all tests
    - watchers_bins_per_test.csv       # one row per test with raw counts per bin
    - variables_bins_per_test.csv      # one row per test with raw counts per bin
"""

import sys
from pathlib import Path
from unified_parser import parse_log_directory
import csv


def collect_results(log_dir: Path):
    """Collect parsed results from a directory, supporting multi-seed layout.
    No stdout chatter; caller decides on messaging.
    """
    if not log_dir.exists():
        return []

    seed_dirs = sorted([d for d in log_dir.glob('seed*') if d.is_dir()])
    all_results = []

    if seed_dirs:
        for sd in seed_dirs:
            res = parse_log_directory(sd, exclude_summary=True)
            if res:
                all_results.extend(res)
    else:
        all_results = parse_log_directory(log_dir, exclude_summary=True)

    # Include SAT/UNSAT/UNKNOWN
    finished = [r for r in all_results if r.get('result') in ('SAT', 'UNSAT', 'UNKNOWN')]
    return finished


def aggregate_bins(results, histogram_key: str):
    """Aggregate bin counts across all results for the given histogram key.

    Returns a dict mapping bin_key (as string) -> total_samples.
    """
    totals = {}

    for r in results:
        bins = r.get(histogram_key, {}) or {}
        for bin_key, values in bins.items():
            samples = values.get('samples', 0) or 0
            if samples == 0:
                continue
            # Normalize key representation to string for consistent printing
            if isinstance(bin_key, int):
                key_str = str(bin_key)
            else:
                key_str = str(bin_key)
            totals[key_str] = totals.get(key_str, 0) + samples

    return totals


def sort_bin_keys(keys):
    """Sort keys with the following order:
      1) numeric bins ascending (e.g., '1', '2', ...)
      2) range bins by start value ascending (e.g., '3-7')
      3) 'out_of_bounds' last
      4) any other strings after that (stable)
    """
    def key_fn(k: str):
        if k == 'out_of_bounds':
            return (3, float('inf'))
        if '-' in k:
            try:
                start = int(k.split('-')[0])
            except ValueError:
                start = float('inf')
            return (2, start)
        try:
            return (1, int(k))
        except ValueError:
            return (4, k)

    return sorted(keys, key=key_fn)


def collect_all_bin_keys(results, histogram_key: str):
    """Return sorted union of all bin keys observed for the histogram key."""
    all_keys = set()
    for r in results:
        bins = r.get(histogram_key, {}) or {}
        for k in bins.keys():
            all_keys.add(str(k))
    return sort_bin_keys(list(all_keys))


def write_csv(out_path: Path, totals: dict):
    """Write aggregated totals to CSV with columns: bin, count, percent."""
    ordered_keys = sort_bin_keys(list(totals.keys())) if totals else []
    grand_total = sum(totals.values()) if totals else 0

    with out_path.open('w', newline='') as f:
        writer = csv.writer(f)
        writer.writerow(['bin', 'count', 'percent'])
        for k in ordered_keys:
            cnt = totals[k]
            pct = (cnt / grand_total * 100.0) if grand_total > 0 else 0.0
            writer.writerow([k, cnt, f"{pct:.6f}"])
        # Optionally include a total row for reference
        writer.writerow(['TOTAL', grand_total, '100.000000' if grand_total > 0 else '0.000000'])


def choose_test_id(meta: dict, fallback: str) -> str:
    """Pick a stable ID for a test row from common fields, else fallback."""
    for key in (
        'benchmark','instance','problem','cnf','source',
        'log_path', 'logfile', 'log_file', 'file', 'filename', 'name', 'test', 'case'):
        val = meta.get(key)
        if isinstance(val, str) and val:
            return val
    return fallback


def write_per_test_csv(out_path: Path, results, histogram_key: str, ordered_keys: list[str]):
    """Write one row per test with raw bin counts for the given histogram.
    Columns: test_id, result, then one column per ordered bin key.
    """
    with out_path.open('w', newline='') as f:
        writer = csv.writer(f)
        header = ['test_id', 'result'] + ordered_keys
        writer.writerow(header)
        for idx, r in enumerate(results):
            test_id = choose_test_id(r, f'test_{idx:05d}')
            result = r.get('result', '')
            bins = r.get(histogram_key, {}) or {}
            row = [test_id, result]
            for k in ordered_keys:
                v = bins.get(k, bins.get(int(k), {})) if k.isdigit() else bins.get(k, {})
                cnt = 0
                if isinstance(v, dict):
                    cnt = int(v.get('samples', 0) or 0)
                elif isinstance(v, (int, float)):
                    cnt = int(v)
                row.append(cnt)
            writer.writerow(row)


def main():
    if len(sys.argv) < 2:
        print("Usage: python tools/parse_histogram.py <logs_folder> [output_dir]")
        sys.exit(1)

    logs_folder = Path(sys.argv[1])
    out_dir = Path(sys.argv[2]) if len(sys.argv) > 2 else Path('.')
    out_dir.mkdir(parents=True, exist_ok=True)
    results = collect_results(logs_folder)
    if not results:
        # Silent exit if nothing to do per user's request to not print
        sys.exit(1)

    watchers = aggregate_bins(results, 'watchers_bins')
    variables = aggregate_bins(results, 'variables_bins')

    # Per-test CSVs (union of observed keys across tests)
    watchers_keys = collect_all_bin_keys(results, 'watchers_bins')
    variables_keys = collect_all_bin_keys(results, 'variables_bins')
    write_per_test_csv(out_dir / 'watchers_bins.csv', results, 'watchers_bins', watchers_keys)
    write_per_test_csv(out_dir / 'literal_bins.csv', results, 'variables_bins', variables_keys)

    print(f"Dumping files: {out_dir / 'watchers_bins.csv'}, {out_dir / 'literal_bins.csv'}")


if __name__ == '__main__':
    main()
