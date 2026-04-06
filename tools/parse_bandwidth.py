#!/usr/bin/env python3
"""
Parse SAT solver result folders and compute memory bandwidth statistics.

Computes L1, L2, and DDR bandwidth in GB/s from aggregate cache statistics
(which include cold misses and prefetch traffic) and simulated time.

Usage:
    python parse_bandwidth.py <result_folder>
    python parse_bandwidth.py <result_folder> --per-test
"""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from unified_parser import parse_log_directory

CACHE_LINE_SIZE = 64  # bytes, same for L1 and L2


def compute_bandwidth(result):
    """Compute L1, L2, and DDR bandwidth in GB/s from a parsed log result.

    Uses aggregate cache statistics (agg_* fields) which include cold misses
    and prefetch traffic. These are int counts, not float percentages.
    """
    if result.get('result') in ('TIMEOUT', 'ERROR', 'UNKNOWN'):
        return None

    sim_time_ms = result.get('sim_time_ms', 0)
    if not sim_time_ms or sim_time_ms <= 0:
        return None

    l1_requests = result.get('agg_l1_total_requests')
    l1_misses = result.get('agg_l1_misses')
    l2_misses = result.get('agg_l2_misses')

    if l1_requests is None or l1_misses is None or l2_misses is None:
        return None

    sim_time_s = sim_time_ms / 1000.0

    return {
        'test_case': result.get('test_case', ''),
        'l1_bw_GBs': l1_requests * CACHE_LINE_SIZE / sim_time_s / 1e9,
        'l2_bw_GBs': l1_misses * CACHE_LINE_SIZE / sim_time_s / 1e9,
        'ddr_bw_GBs': l2_misses * CACHE_LINE_SIZE / sim_time_s / 1e9,
    }


def parse_result_folder(folder_path):
    """Parse all logs in a result folder (with or without seed subdirectories)."""
    folder_path = Path(folder_path)
    seed_dirs = sorted([p for p in folder_path.glob('seed*') if p.is_dir()])

    all_bandwidths = []
    num_seeds = 0

    if seed_dirs:
        for sd in seed_dirs:
            num_seeds += 1
            results = parse_log_directory(sd, exclude_summary=True)
            for r in results:
                bw = compute_bandwidth(r)
                if bw is not None:
                    bw['seed'] = sd.name
                    all_bandwidths.append(bw)
    else:
        num_seeds = 1
        results = parse_log_directory(folder_path, exclude_summary=True)
        for r in results:
            bw = compute_bandwidth(r)
            if bw is not None:
                all_bandwidths.append(bw)

    return all_bandwidths, num_seeds


def print_bandwidth_summary(bandwidths, num_seeds, per_test=False):
    """Print avg, min, max bandwidth statistics."""
    if not bandwidths:
        print("No valid bandwidth data found.")
        return

    l1_vals = [b['l1_bw_GBs'] for b in bandwidths]
    l2_vals = [b['l2_bw_GBs'] for b in bandwidths]
    ddr_vals = [b['ddr_bw_GBs'] for b in bandwidths]

    n = len(bandwidths)
    test_cases = set(b['test_case'] for b in bandwidths)

    print(f"\nMemory Bandwidth Summary ({len(test_cases)} test cases, {n} total runs across {num_seeds} seed(s))")
    print("=" * 64)
    print(f"{'':18s} {'Avg (GB/s)':>12s} {'Min (GB/s)':>12s} {'Max (GB/s)':>12s}")
    print("-" * 64)

    for label, vals in [("L1 bandwidth", l1_vals), ("L2 bandwidth", l2_vals), ("DDR bandwidth", ddr_vals)]:
        avg = sum(vals) / len(vals)
        print(f"{label:18s} {avg:12.4f} {min(vals):12.4f} {max(vals):12.4f}")

    print("=" * 64)

    if per_test:
        print(f"\nPer-test breakdown:")
        print(f"{'Test Case':60s} {'L1 (GB/s)':>10s} {'L2 (GB/s)':>10s} {'DDR (GB/s)':>10s}")
        print("-" * 94)
        for b in sorted(bandwidths, key=lambda x: x['ddr_bw_GBs'], reverse=True):
            seed_str = f" [{b['seed']}]" if 'seed' in b else ""
            name = b['test_case'] + seed_str
            if len(name) > 58:
                name = "..." + name[-55:]
            print(f"{name:60s} {b['l1_bw_GBs']:10.4f} {b['l2_bw_GBs']:10.4f} {b['ddr_bw_GBs']:10.4f}")


def main():
    parser = argparse.ArgumentParser(
        description='Compute memory bandwidth statistics from SAT solver result folders')
    parser.add_argument('result_folder', help='Path to result folder (with seed*/ subdirs or flat)')
    parser.add_argument('--per-test', action='store_true', help='Print per-test-case breakdown')
    args = parser.parse_args()

    folder = Path(args.result_folder)
    if not folder.is_dir():
        print(f"Error: {folder} is not a directory")
        sys.exit(1)

    bandwidths, num_seeds = parse_result_folder(folder)
    print_bandwidth_summary(bandwidths, num_seeds, per_test=args.per_test)


if __name__ == '__main__':
    main()
