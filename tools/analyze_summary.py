#!/usr/bin/env python3
"""
Analyze a SAT runs summary.log to find incomplete (test, seed) pairs and
print safe removal commands for their .log and .stats.csv files.

Usage:
  python scripts/analyze_summary.py --run-dir runs/opt_l1_4_1_l2_8_32
  # or
  python scripts/analyze_summary.py --summary runs/opt_l1_4_1_l2_8_32/summary.log

The script will:
- Parse Number of random seeds and Found <N> benchmark files from the summary
- Parse all lines like "<test> (seed S): PASSED/FAILED (...)"
- Infer the complete set of tests from seed folders (seed0..seedN-1) if present,
  otherwise from the tests that appear in the summary.
- Report counts and list each missing (test, seed)
- Emit an "echo rm -f ..." preview block. Re-run with --no-dry-run to actually print rm commands
"""
import argparse
import re
import sys
from pathlib import Path
from collections import defaultdict

LINE_RE = re.compile(r"^(.*) \(seed (\d+)\): (PASSED|FAILED)\b")
SEEDS_RE = re.compile(r"^Number of random seeds: (\d+)\b")
TESTCOUNT_RE = re.compile(r"^Found (\d+) benchmark files to test\b")


def parse_summary(summary_path: Path):
    seeds_declared = None
    tests_declared = None
    completed = defaultdict(set)  # test -> set(seeds)
    with summary_path.open('r', encoding='utf-8', errors='ignore') as f:
        for raw in f:
            line = raw.strip()
            m = SEEDS_RE.match(line)
            if m:
                seeds_declared = int(m.group(1))
                continue
            m = TESTCOUNT_RE.match(line)
            if m:
                tests_declared = int(m.group(1))
                continue
            m = LINE_RE.match(line)
            if m:
                test, seed = m.group(1), int(m.group(2))
                completed[test].add(seed)
    return seeds_declared, tests_declared, completed


def infer_all_tests(run_dir: Path, completed_tests):
    """Return a sorted set of all test basenames.
    Prefer reading from seed directories; fallback to completed_tests.
    """
    seed_dirs = sorted([p for p in run_dir.iterdir() if p.is_dir() and p.name.startswith('seed')])
    tests = set()
    for sd in seed_dirs:
        for f in sd.iterdir():
            # accept .log or .stats.csv; strip suffix(es) to base test name
            name = f.name
            if name.endswith('.stats.csv'):
                base = name[:-len('.stats.csv')]
            elif name.endswith('.log'):
                base = name[:-len('.log')]
            else:
                continue
            tests.add(base)
    if not tests:
        tests = set(completed_tests)
    return sorted(tests)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--run-dir', type=Path, default=Path('runs/opt_l1_4_1_l2_8_32'), help='Path to a run directory containing summary.log and seed*/ subdirs')
    ap.add_argument('--summary', type=Path, help='Path to a summary.log (overrides --run-dir/summary.log)')
    ap.add_argument('--no-dry-run', action='store_true', help='Print rm commands instead of echo (use with caution)')
    args = ap.parse_args()

    run_dir = args.run_dir
    summary_path = args.summary if args.summary else (run_dir / 'summary.log')

    if not summary_path.exists():
        print(f"ERROR: Cannot find summary.log at {summary_path}", file=sys.stderr)
        sys.exit(2)

    seeds_declared, tests_declared, completed = parse_summary(summary_path)
    if seeds_declared is None:
        seeds_declared = 4  # sensible default
    seeds = list(range(seeds_declared))

    all_tests = infer_all_tests(run_dir, completed_tests=completed.keys())

    completed_pairs = sum(len(v) for v in completed.values())
    expected_pairs = len(all_tests) * len(seeds)

    print(f"Summary path: {summary_path}")
    print(f"Run dir:      {run_dir}")
    print(f"Declared seeds: {seeds_declared}")
    if tests_declared is not None:
        print(f"Declared tests: {tests_declared}")
    print(f"Inferred tests: {len(all_tests)}")
    print(f"Completed pairs: {completed_pairs}")
    print(f"Expected pairs:  {expected_pairs}")
    missing = []
    for test in all_tests:
        done = completed.get(test, set())
        for s in seeds:
            if s not in done:
                missing.append((test, s))
    print(f"Missing pairs:   {len(missing)}")
    if tests_declared is not None and tests_declared != len(all_tests):
        print(f"NOTE: Declared {tests_declared} tests but inferred {len(all_tests)} from seed folders/summary; proceeding with inferred set.")

    # Print missing list
    print("\nMissing (test, seed):")
    for test, s in missing:
        print(f"  {test} (seed {s})")

    # Emit removal commands
    print("\nRemoval commands (preview):")
    rm_prefix = '' if args.no_dry_run else 'echo '
    for test, s in missing:
        base = run_dir / f"seed{s}" / test
        print(f"{rm_prefix}rm -f '{base}.log' '{base}.stats.csv'")


if __name__ == '__main__':
    main()
