#!/usr/bin/env python3
"""
Cleanup TIMEOUT test artifacts.

Parses a summary.log produced by the SAT run framework, identifies lines
containing "TIMEOUT", extracts the test base filename and seed, and deletes
corresponding .log and .stats.csv files under seed directories.

Usage:
  python scripts/cleanup_timeouts.py --summary runs/opt_l1_4_1_l2_8_32/summary.log --runs-root runs/opt_l1_4_1_l2_8_32 --dry-run
  python scripts/cleanup_timeouts.py --summary ... --runs-root ... --delete

Flags:
  --summary    Path to summary.log
  --runs-root  Path to runs root that contains seedN subdirectories
  --dry-run    Only list files that would be removed (default behavior if neither --dry-run nor --delete provided)
  --delete     Perform deletion of artifacts
  --expect     Expected number of TIMEOUT entries (optional, will warn if mismatch)

Exit codes:
  0 success
  2 summary file not found
  3 runs-root not found
  4 parsing error
"""
from __future__ import annotations
import argparse
import os
import re
import sys
from dataclasses import dataclass
from typing import List, Tuple

TIMEOUT_PATTERN = re.compile(r"^(?P<name>.+?) \(seed (?P<seed>\d+)\): TIMEOUT")

@dataclass
class TimeoutEntry:
    name: str
    seed: int

    @property
    def base(self) -> str:
        return self.name

    def artifact_paths(self, runs_root: str) -> Tuple[str, str]:
        seed_dir = os.path.join(runs_root, f"seed{self.seed}")
        log_path = os.path.join(seed_dir, f"{self.base}.log")
        stats_path = os.path.join(seed_dir, f"{self.base}.stats.csv")
        return log_path, stats_path


def parse_timeouts(summary_path: str) -> List[TimeoutEntry]:
    entries: List[TimeoutEntry] = []
    try:
        with open(summary_path, 'r', encoding='utf-8', errors='ignore') as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                if 'TIMEOUT' not in line:
                    continue
                m = TIMEOUT_PATTERN.match(line)
                if m:
                    name = m.group('name')
                    seed = int(m.group('seed'))
                    entries.append(TimeoutEntry(name=name, seed=seed))
                # If pattern does not match, ignore (could be other log noise)
    except OSError as e:
        print(f"ERROR: Unable to read summary file: {e}", file=sys.stderr)
        sys.exit(2)
    return entries


def delete_artifacts(entries: List[TimeoutEntry], runs_root: str, dry_run: bool) -> Tuple[int, int, List[str]]:
    deleted = 0
    missing = 0
    missing_files: List[str] = []
    for e in entries:
        log_path, stats_path = e.artifact_paths(runs_root)
        for p in (log_path, stats_path):
            if os.path.exists(p):
                if dry_run:
                    print(f"WOULD DELETE: {p}")
                else:
                    try:
                        os.remove(p)
                        print(f"DELETED: {p}")
                        deleted += 1
                    except OSError as err:
                        print(f"ERROR deleting {p}: {err}", file=sys.stderr)
            else:
                print(f"MISSING: {p}")
                missing += 1
                missing_files.append(p)
    return deleted, missing, missing_files


def main():
    ap = argparse.ArgumentParser(description="Remove artifacts for TIMEOUT tests.")
    ap.add_argument('--summary', required=True, help='Path to summary.log file')
    ap.add_argument('--runs-root', required=True, help='Runs root containing seed directories')
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--dry-run', action='store_true', help='List files only (default)')
    g.add_argument('--delete', action='store_true', help='Perform deletion')
    ap.add_argument('--expect', type=int, default=None, help='Expected number of TIMEOUT entries to verify')
    args = ap.parse_args()

    if not os.path.isfile(args.summary):
        print(f"ERROR: summary file not found: {args.summary}", file=sys.stderr)
        sys.exit(2)
    if not os.path.isdir(args.runs_root):
        print(f"ERROR: runs-root directory not found: {args.runs_root}", file=sys.stderr)
        sys.exit(3)

    entries = parse_timeouts(args.summary)

    if args.expect is not None and args.expect != len(entries):
        print(f"WARNING: Expected {args.expect} TIMEOUT entries but found {len(entries)}.")

    if not entries:
        print("No TIMEOUT entries found. Nothing to do.")
        return

    print(f"Found {len(entries)} TIMEOUT entries.")
    dry_run = not args.delete or args.dry_run  # default to dry-run if neither specified
    mode = 'DRY-RUN' if dry_run else 'DELETE'
    print(f"Mode: {mode}")

    deleted, missing, missing_files = delete_artifacts(entries, args.runs_root, dry_run)

    print("\nSummary:")
    print(f"  TIMEOUT entries: {len(entries)}")
    if dry_run:
        print(f"  Files that would be deleted: {len(entries)*2 - missing}")
    else:
        print(f"  Files deleted: {deleted}")
    print(f"  Missing files referenced: {missing}")
    if missing_files:
        print("  Missing file list:")
        for m in missing_files:
            print(f"    {m}")

    # Provide quick re-run hint
    seeds = sorted({e.seed for e in entries})
    print("\nRe-run hint: invoke your test harness restricted to these seeds and test names.")
    print("Seeds:", ",".join(map(str, seeds)))
    print("Test bases (one per line):")
    for e in entries:
        print(e.base)

if __name__ == '__main__':
    main()
