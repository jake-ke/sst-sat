#!/usr/bin/env python3
"""
Compute coprocessor mode cycle estimates from solver log files.

Uses raw statistics collected during simulation (with --coprocessor-mode 1)
and existing HW cycle counters to estimate coprocessor performance offline
with arbitrary roundtrip and sf_scale parameters.

Usage:
    python compute_coprocessor.py <log_file_or_dir> [--roundtrip N] [--sf-scale F] [--csv]
"""

import argparse
import sys
import os
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unified_parser import parse_satsolver_log, parse_log_directory


def compute_coprocessor(result, roundtrip=10, sf_scale=1.0):
    """Compute coprocessor cycle estimates for each merged phase.

    Returns dict with phase names -> estimated cycles, plus hw_ counterparts.
    """
    # HW cycles from existing counters (constant-sf phases)
    hw_prop = result.get('propagate_cycles', 0) + result.get('heap_insert_cycles', 0) + result.get('heap_bump_cycles', 0)
    hw_learn = result.get('analyze_cycles', 0)
    hw_min = result.get('minimize_cycles', 0)
    hw_bt = result.get('backtrack_cycles', 0) + result.get('restart_cycles', 0)
    hw_del = result.get('reduce_db_cycles', 0)
    hw_dec = result.get('decision_cycles', 0)

    # Propagation invocations (derivable from existing stats)
    prop_invocations = result.get('decisions', 0) + result.get('restarts', 0) + result.get('db_reductions', 0) + 1

    # Coprocessor cost per phase
    coproc = {
        'propagation': hw_prop + 2 * prop_invocations * roundtrip,
        'learning':    result.get('coproc_sf_hw_learning', 0) * sf_scale + result.get('coproc_dep_learning', 0) * roundtrip,
        'minimize':    result.get('coproc_sf_hw_minimize', 0) * sf_scale + result.get('coproc_dep_minimize', 0) * roundtrip,
        'backtrack':   hw_bt * 2.0 * sf_scale + result.get('coproc_dep_backtrack', 0) * roundtrip,
        'deletion':    hw_del * 1.5 * sf_scale,
        'decision':    hw_dec * 3.0 * sf_scale + result.get('coproc_dep_decision', 0) * roundtrip,
    }

    # Corresponding HW cycles for comparison
    hw = {
        'propagation': hw_prop,
        'learning':    hw_learn,
        'minimize':    hw_min,
        'backtrack':   hw_bt,
        'deletion':    hw_del,
        'decision':    hw_dec,
    }

    return coproc, hw


def print_table(test_case, coproc, hw):
    """Print a formatted coprocessor comparison table."""
    coproc_total = sum(coproc.values())
    hw_total = sum(hw.values())

    labels = {
        'propagation': 'Propagation',
        'learning':    'Clause Learning',
        'minimize':    'Minimization',
        'backtrack':   'Backtrack',
        'deletion':    'Deletion',
        'decision':    'Decision',
    }

    print(f"\n--- {test_case} ---")
    print(f"{'Phase':<17} {'Coproc Cycles':>15} {'%':>8} {'HW Cycles':>15}")
    for key in ['propagation', 'learning', 'minimize', 'backtrack', 'deletion', 'decision']:
        cc = coproc[key]
        hc = hw[key]
        pct = cc / coproc_total * 100.0 if coproc_total > 0 else 0.0
        print(f"{labels[key]:<17} {cc:>15.0f} {pct:>7.2f}% {hc:>15}")
    print(f"{'-'*57}")
    print(f"{'Total':<17} {coproc_total:>15.0f} {'100.00%':>8} {hw_total:>15}")
    if hw_total > 0:
        print(f"Coproc / HW    : {coproc_total / hw_total:.2f}x")


def print_csv_header():
    """Print CSV header row."""
    fields = [
        'test_case',
        'hw_total', 'coproc_total', 'coproc_hw_ratio',
        'hw_propagation', 'hw_learning', 'hw_minimize', 'hw_backtrack', 'hw_deletion', 'hw_decision',
        'coproc_propagation', 'coproc_learning', 'coproc_minimize', 'coproc_backtrack', 'coproc_deletion', 'coproc_decision',
        'coproc_propagation_pct', 'coproc_learning_pct', 'coproc_minimize_pct', 'coproc_backtrack_pct', 'coproc_deletion_pct', 'coproc_decision_pct',
    ]
    print(','.join(fields))


def print_csv_row(test_case, coproc, hw):
    """Print a single CSV data row."""
    coproc_total = sum(coproc.values())
    hw_total = sum(hw.values())
    ratio = coproc_total / hw_total if hw_total > 0 else 0.0

    phases = ['propagation', 'learning', 'minimize', 'backtrack', 'deletion', 'decision']
    hw_vals = [str(hw[p]) for p in phases]
    cc_vals = [f"{coproc[p]:.0f}" for p in phases]
    pct_vals = [f"{coproc[p] / coproc_total * 100.0:.2f}" if coproc_total > 0 else "0.00" for p in phases]

    row = [test_case, str(hw_total), f"{coproc_total:.0f}", f"{ratio:.4f}"] + hw_vals + cc_vals + pct_vals
    print(','.join(row))


def process_log(log_path, roundtrip, sf_scale, csv_mode):
    """Process a single log file."""
    content = Path(log_path).read_text()
    result = parse_satsolver_log(str(log_path), content)

    # Check if coprocessor stats are present
    has_coproc = any(k.startswith('coproc_') for k in result)
    if not has_coproc:
        if not csv_mode:
            print(f"Warning: {log_path} has no coprocessor raw statistics (was --coprocessor-mode 1 used?)")
        return None

    coproc, hw = compute_coprocessor(result, roundtrip, sf_scale)
    test_case = result.get('test_case', os.path.basename(log_path))

    if csv_mode:
        print_csv_row(test_case, coproc, hw)
    else:
        print_table(test_case, coproc, hw)

    return coproc, hw


def main():
    parser = argparse.ArgumentParser(
        description='Compute coprocessor mode cycle estimates from solver log files',
        formatter_class=argparse.ArgumentDefaultsHelpFormatter
    )
    parser.add_argument('path', help='Log file or directory containing log files')
    parser.add_argument('--roundtrip', type=int, default=10,
                        help='Embedded CPU round-trip to on-chip SRAM in accel cycles')
    parser.add_argument('--sf-scale', type=float, default=1.0,
                        help='Overall scaling factor for CPU serialization factors')
    parser.add_argument('--csv', action='store_true',
                        help='Output as CSV instead of formatted tables')
    args = parser.parse_args()

    path = Path(args.path)

    if args.csv:
        print_csv_header()

    if path.is_file():
        result = process_log(path, args.roundtrip, args.sf_scale, args.csv)
        if result is None and not args.csv:
            sys.exit(1)
    elif path.is_dir():
        # Find all .log files recursively
        log_files = sorted(path.rglob('*.log'))
        if not log_files:
            print(f"No .log files found in {path}", file=sys.stderr)
            sys.exit(1)

        all_coproc = []
        all_hw = []
        for log_file in log_files:
            if log_file.name == 'summary.log':
                continue
            result = process_log(log_file, args.roundtrip, args.sf_scale, args.csv)
            if result is not None:
                coproc, hw = result
                all_coproc.append(coproc)
                all_hw.append(hw)

        # Print aggregate summary
        if not args.csv and all_coproc:
            phases = ['propagation', 'learning', 'minimize', 'backtrack', 'deletion', 'decision']
            agg_coproc = {p: sum(c[p] for c in all_coproc) for p in phases}
            agg_hw = {p: sum(h[p] for h in all_hw) for p in phases}

            print(f"\n{'='*57}")
            print(f"AGGREGATE ({len(all_coproc)} tests, roundtrip={args.roundtrip}, sf_scale={args.sf_scale})")
            print_table("AGGREGATE", agg_coproc, agg_hw)
    else:
        print(f"Path not found: {path}", file=sys.stderr)
        sys.exit(1)


if __name__ == '__main__':
    main()
