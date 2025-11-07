#!/usr/bin/env python3
"""
SAT Solver Result Parser

This script parses log files from SAT solver runs and generates CSV reports.

Enhancements:
- Reads matching <test_case>.stats.csv to extract L1 prefetch requests/drops
    from the last occurrence of rows starting with
    "global_l1cache,Prefetch_requests," and "global_l1cache,Prefetch_drops,"
    and records their Sum.u64 (column 7 in the CSV header) per test.
- Adds prefetch statistics and propagation detail statistics to the CSV.

Usage: python parse_results.py <results_folder> [output_file]
"""

import sys
import csv
from pathlib import Path
from unified_parser import parse_log_directory, format_bytes


# CSV prefetch parsing is handled in unified_parser now.


def write_csv_report(results, output_file, *, par2_score_seconds=None, solved_count=None, total_problems=None):
    """Write detailed results to a CSV file with dynamic fields for extras.

    The CSV rows are sorted by test_case name. If par2_score_seconds and
    solved_count are provided, a final summary row is appended with these
    metrics encoded in the 'result' column.
    """
    # Sort results by test case name before writing
    sorted_results = sorted(results, key=lambda x: x.get('test_case', ''))

    # Base columns: basic info, solver stats, L1 totals, then L1 components (total+miss%), then cycles (+ percentages)
    base_fields = [
        'test_case', 'result', 'variables', 'clauses',
        'total_memory_bytes', 'total_memory_formatted', 'sim_time_ms',
        # Solver statistics
        'decisions', 'propagations', 'conflicts', 'learned', 'removed',
        'db_reductions', 'minimized', 'restarts', 'spec_started', 'spec_finished',
        # L1 cache totals first
        'l1_total_requests', 'l1_total_miss_rate',
        # L1 cache by data structure
        'l1_heap_total', 'l1_heap_miss_rate',
        'l1_variables_total', 'l1_variables_miss_rate',
        'l1_watches_total', 'l1_watches_miss_rate',
        'l1_clauses_total', 'l1_clauses_miss_rate',
        'l1_varactivity_total', 'l1_varactivity_miss_rate',
        # Cycle statistics
        'propagate_cycles', 'analyze_cycles', 'minimize_cycles', 'backtrack_cycles',
        'decision_cycles', 'reduce_db_cycles', 'restart_cycles', 'total_counted_cycles',
        # Cycle percentages (computed)
        'propagate_cycles_pct', 'analyze_cycles_pct', 'minimize_cycles_pct', 'backtrack_cycles_pct',
        'decision_cycles_pct', 'reduce_db_cycles_pct', 'restart_cycles_pct'
    ]

    # Extra fixed fields: directed prefetcher stats and CSV prefetch requests/drops
    extra_fixed = [
        'prefetches_issued', 'prefetches_used', 'prefetches_unused', 'prefetch_accuracy',
        'l1_prefetch_requests', 'l1_prefetch_drops', 'l1_prefetch_drop_pct'
    ]

    # Add Watcher Blocks Visited distribution (percentages)
    wbv_fields = [
        'watcher_blocks_visited_1_pct',
        'watcher_blocks_visited_2_pct',
        'watcher_blocks_visited_3_pct',
        'watcher_blocks_visited_gt3_pct',
    ]

    # Dynamic propagation detail fields (union across results)
    prop_fields = sorted({k for r in results for k in r.keys() if k.startswith('prop_')})

    fieldnames = base_fields + extra_fixed + wbv_fields + prop_fields

    with open(output_file, 'w', newline='') as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()

        cycle_names = [
            'propagate_cycles', 'analyze_cycles', 'minimize_cycles', 'backtrack_cycles',
            'decision_cycles', 'reduce_db_cycles', 'restart_cycles'
        ]

        for result in sorted_results:
            row = {field: result.get(field, 0) for field in fieldnames}

            # Compute cycle percentages if total_counted_cycles present
            total_cycles = result.get('total_counted_cycles', 0) or 0
            for name in cycle_names:
                pct_field = name.replace('_cycles', '_cycles_pct')
                cycles = result.get(name, 0) or 0
                row[pct_field] = (cycles / total_cycles * 100.0) if total_cycles > 0 else 0.0

            # Compute prefetch drop percentage if requests present
            req = result.get('l1_prefetch_requests', 0) or 0
            drops = result.get('l1_prefetch_drops', 0) or 0
            row['l1_prefetch_drop_pct'] = (drops / req * 100.0) if req > 0 else 0.0

            # Compute Watcher Blocks Visited distribution percentages
            wbv_bins = result.get('watcher_blocks_visited_bins', {}) or {}
            def pct_for(key):
                v = wbv_bins.get(key)
                return float(v.get('percentage')) if isinstance(v, dict) and 'percentage' in v else 0.0

            row['watcher_blocks_visited_1_pct'] = pct_for(1)
            row['watcher_blocks_visited_2_pct'] = pct_for(2)
            row['watcher_blocks_visited_3_pct'] = pct_for(3)

            gt3_pct = 0.0
            for k, v in wbv_bins.items():
                # Include Out of bounds values in >3 bucket
                if k == 'out_of_bounds':
                    gt3_pct += float(v.get('percentage', 0.0))
                if isinstance(k, int) and k >= 4:
                    gt3_pct += float(v.get('percentage', 0.0))
                elif isinstance(k, str) and '-' in k:
                    start = int(k.split('-')[0])
                    if start >= 4:
                        gt3_pct += float(v.get('percentage', 0.0))
            row['watcher_blocks_visited_gt3_pct'] = gt3_pct

            writer.writerow(row)

        # Append a final summary row with PAR-2 and solved score if available
        if par2_score_seconds is not None and solved_count is not None and total_problems is not None:
            summary_row = {field: '' for field in fieldnames}
            summary_row.update({field: 0 for field in fieldnames if field not in ('test_case', 'result')})
            summary_row['test_case'] = 'SOLVED'
            summary_row['result'] = f"{solved_count}/{total_problems}"
            summary_row['variables'] = 'PAR-2'
            summary_row['clauses'] = f"{par2_score_seconds:.2f} s"
            writer.writerow(summary_row)


def parse_results_folder(folder_path, output_file=None, timeout_seconds=3600, dump_best=False):
    """Parse all log files in the given folder and generate a report.
    
    Supports multi-seed folders: if the folder contains subfolders named
    seed*, each seed folder is parsed and results are averaged across seeds
    for reporting and CSV output.
    
    Args:
        folder_path: Path to folder containing log files or seed* subfolders
        output_file: Optional path to write CSV report
        timeout_seconds: Timeout in seconds for PAR-2 calculation (default: 3600)
    """
    folder_path = Path(folder_path)
    
    if not folder_path.exists():
        print(f"Error: Folder {folder_path} does not exist")
        return

    # Detect multi-seed layout
    seed_dirs = [p for p in sorted(folder_path.glob('seed*')) if p.is_dir()]

    def _get_last_stats_time_ms(stats_dir: Path, test_case: str) -> float:
        """Return the last statistics dump time in ms from <test_case>.stats.csv.

        The stats CSV is expected to have a simulated time column in picoseconds.
        Common headers include 'SimTimeps' or variants. We try a small set of
        candidate column names and take the maximum value across rows.
        """
        try:
            stats_csv = stats_dir / f"{test_case}.stats.csv"
            if not stats_csv.exists() or not stats_csv.is_file():
                return 0.0
            last_ps = 0.0
            with stats_csv.open('r', newline='') as f:
                reader = csv.DictReader(f)
                # Candidate column names for simulated time in ps
                candidates = (
                    'SimTimeps', 'SimTime', 'SimTime_ps', 'SimTime(ps)'
                )
                for row in reader:
                    ps_val = None
                    for key in candidates:
                        if key in row and row.get(key) not in (None, ''):
                            try:
                                ps_val = float(row.get(key))
                            except (TypeError, ValueError):
                                ps_val = None
                            if ps_val is not None:
                                break
                    if ps_val is not None:
                        if ps_val > last_ps:
                            last_ps = ps_val
            # Convert picoseconds to milliseconds: 1 ms = 1e9 ps
            return last_ps / 1e9
        except Exception:
            return 0.0

    def _compute_par2_and_solved(res_list):
        """Return (par2_seconds, solved_count) for a list of parsed results."""
        if not res_list:
            return 0.0, 0
        timeout_ms = timeout_seconds * 1000.0
        par2_penalty = 2 * timeout_ms
        par2_total = 0.0
        solved = 0
        for r in res_list:
            sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
            if r.get('result') in ('SAT', 'UNSAT') and sim_ms <= timeout_ms:
                par2_total += sim_ms
                solved += 1
            else:
                # Unknown or exceeded timeout -> treat as timeout penalty
                par2_total += par2_penalty
        return (par2_total / len(res_list)) / 1000.0, solved

    def _avg_of(values):
        vals = [v for v in values if v is not None]
        if not vals:
            return 0
        return sum(vals) / len(vals)

    # Numeric fields to average when aggregating across seeds
    numeric_fields = [
        'variables', 'clauses', 'total_memory_bytes', 'sim_time_ms',
        'decisions', 'propagations', 'conflicts', 'learned', 'removed',
        'db_reductions', 'minimized', 'restarts',
        'l1_total_requests', 'l1_total_miss_rate',
        'l1_heap_total', 'l1_heap_miss_rate',
        'l1_variables_total', 'l1_variables_miss_rate',
        'l1_watches_total', 'l1_watches_miss_rate',
        'l1_clauses_total', 'l1_clauses_miss_rate',
        'l1_varactivity_total', 'l1_varactivity_miss_rate',
        'propagate_cycles', 'analyze_cycles', 'minimize_cycles', 'backtrack_cycles',
        'decision_cycles', 'reduce_db_cycles', 'restart_cycles', 'total_counted_cycles',
        'prefetches_issued', 'prefetches_used', 'prefetches_unused', 'prefetch_accuracy',
        'l1_prefetch_requests', 'l1_prefetch_drops'
    ]

    # If seeds exist and contain logs, parse each seed and aggregate
    if seed_dirs:
        seed_results = []  # list of lists
        for sd in seed_dirs:
            res = parse_log_directory(sd, exclude_summary=True)
            if res:
                # Apply timeout classification before further aggregation
                timeout_ms = timeout_seconds * 1000.0
                for r in res:
                    try:
                        sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
                    except (TypeError, ValueError):
                        sim_ms = 0.0
                    # If a run exceeded timeout, treat as UNKNOWN to reflect penalty in CSV as well
                    if r.get('result') in ('SAT', 'UNSAT') and sim_ms > timeout_ms:
                        r['result'] = 'UNKNOWN'
                seed_results.append(res)
        if seed_results:
            print(f"Detected {len(seed_results)} seed folders under {folder_path}")
            # Compute per-seed PAR-2 and solved
            per_seed_par2 = []
            per_seed_solved = []
            per_seed_counts = []
            # Build test_case union from logs and also include tests that only have stats CSVs
            all_cases = set()
            for res in seed_results:
                for r in res:
                    all_cases.add(r.get('test_case', ''))
            # Also add any cases that appear only in <test_case>.stats.csv (timeouts may not write logs)
            for sd in seed_dirs:
                for stats_csv in sd.glob('*.stats.csv'):
                    name = stats_csv.name
                    if name.endswith('.stats.csv'):
                        case = name[:-10]  # strip '.stats.csv'
                        if case:
                            all_cases.add(case)

            # Map seed index -> {test_case -> result}
            seed_maps = []
            for res in seed_results:
                m = {r.get('test_case', ''): r for r in res}
                seed_maps.append(m)

            print(f"Per-seed results (timeout: {timeout_seconds}s):")
            for idx, res in enumerate(seed_results):
                par2, solved = _compute_par2_and_solved(res)
                per_seed_par2.append(par2)
                per_seed_solved.append(solved)
                per_seed_counts.append(len(res))
                seed_label = seed_dirs[idx].name if idx < len(seed_dirs) else f"seed{idx}"
                print(f"  {seed_label}: Solved {solved}/{len(res)}, PAR-2: {par2:.2f} s")

            # Aggregate per test_case by averaging numeric fields across seeds where present
            # Averaging rules:
            # - For most numeric fields: average over finished runs only (SAT/UNSAT)
            # - For sim_time_ms: average over ALL seeds, substituting 2*timeout_ms for unfinished (UNKNOWN)
            aggregated_results = []
            timeout_ms = timeout_seconds * 1000.0
            par2_penalty_ms = 2 * timeout_ms

            numeric_fields_excluding_time = [
                'variables', 'clauses', 'total_memory_bytes',
                'decisions', 'propagations', 'conflicts', 'learned', 'removed',
                'db_reductions', 'minimized', 'restarts',
                'l1_total_requests', 'l1_total_miss_rate',
                'l1_heap_total', 'l1_heap_miss_rate',
                'l1_variables_total', 'l1_variables_miss_rate',
                'l1_watches_total', 'l1_watches_miss_rate',
                'l1_clauses_total', 'l1_clauses_miss_rate',
                'l1_varactivity_total', 'l1_varactivity_miss_rate',
                'propagate_cycles', 'analyze_cycles', 'minimize_cycles', 'backtrack_cycles',
                'decision_cycles', 'reduce_db_cycles', 'restart_cycles', 'total_counted_cycles',
                'prefetches_issued', 'prefetches_used', 'prefetches_unused', 'prefetch_accuracy',
                'l1_prefetch_requests', 'l1_prefetch_drops'
            ]
            for case in sorted(all_cases):
                entries = [m[case] for m in seed_maps if case in m]
                if not entries:
                    continue
                agg = {'test_case': case}
                # Determine aggregate result label per request:
                # - We cannot have both SAT and UNSAT, so finished labels should be unanimous when present.
                # - Show SAT/UNSAT along with the number of UNKNOWN seeds if any exist, e.g., "SAT 3".
                # - If all seeds are UNKNOWN, fall back to "UNKNOWN <count>".
                seed_labels = []
                timeout_ms = timeout_seconds * 1000.0
                for e in entries:
                    try:
                        sim_ms = float(e.get('sim_time_ms', 0.0) or 0.0)
                    except (TypeError, ValueError):
                        sim_ms = 0.0
                    # Treat over-time solved runs as UNKNOWN in aggregation labels
                    if e.get('result') in ('SAT','UNSAT') and sim_ms > timeout_ms:
                        seed_labels.append('UNKNOWN')
                    else:
                        seed_labels.append(e.get('result'))
                unknown_count = sum(1 for lbl in seed_labels if lbl not in ('SAT', 'UNSAT'))
                finished_labels = [lbl for lbl in seed_labels if lbl in ('SAT', 'UNSAT')]
                base_label = None
                if finished_labels:
                    # If mixed SAT/UNSAT appears across seeds, mark as ERROR
                    if all(lbl == 'SAT' for lbl in finished_labels):
                        base_label = 'SAT'
                    elif all(lbl == 'UNSAT' for lbl in finished_labels):
                        base_label = 'UNSAT'
                    else:
                        base_label = 'ERROR'

                if base_label is None:
                    # No finished labels; all are UNKNOWN
                    agg['result'] = f"UNKNOWN {unknown_count}" if unknown_count > 0 else 'UNKNOWN'
                else:
                    agg['result'] = f"{base_label} {unknown_count}" if unknown_count > 0 else base_label
                # Finished entries for averaging (exclude UNKNOWN and timeouts that exceeded the limit)
                finished = []
                for e in entries:
                    if e.get('result') in ('SAT', 'UNSAT'):
                        try:
                            if float(e.get('sim_time_ms', 0.0) or 0.0) <= timeout_ms:
                                finished.append(e)
                        except (TypeError, ValueError):
                            # If time can't be parsed, treat as unfinished
                            pass

                # Average non-time numeric fields using finished only
                for key in numeric_fields_excluding_time:
                    vals = []
                    for e in finished:
                        v = e.get(key)
                        if v is None:
                            continue
                        try:
                            vals.append(float(v))
                        except (TypeError, ValueError):
                            continue
                    agg[key] = _avg_of(vals)

                # sim_time_ms uses PAR-2 semantics per seed (2*timeout for unfinished)
                time_vals = []
                for e in entries:
                    try:
                        v = float(e.get('sim_time_ms', 0.0) or 0.0)
                    except (TypeError, ValueError):
                        v = 0.0
                    if e.get('result') in ('SAT', 'UNSAT') and v <= timeout_ms:
                        time_vals.append(v)
                    else:
                        # Unknown or exceeded timeout -> treat as timeout penalty
                        time_vals.append(par2_penalty_ms)
                agg['sim_time_ms'] = _avg_of(time_vals)
                # Memory formatted from averaged bytes
                agg['total_memory_formatted'] = format_bytes(int(agg.get('total_memory_bytes', 0) or 0))
                aggregated_results.append(agg)

            # Print high-level seed-averaged stats
            avg_par2_seconds = _avg_of(per_seed_par2)
            avg_solved = _avg_of(per_seed_solved)
            avg_num_cases = _avg_of([len(r) for r in seed_results])
            total_files = sum(len(r) for r in seed_results)
            print(f"Successfully parsed: {total_files} files across {len(seed_results)} seeds")
            print(f"Average across seeds -> Solved: {avg_solved:.2f}/{avg_num_cases:.2f}, PAR-2: {avg_par2_seconds:.2f} s (timeout: {timeout_seconds}s)")

            # Compute best-of-seeds PAR-2 and solved: for each test case take the minimal PAR-2
            # contribution across seeds (actual sim time if solved within timeout, else 2*timeout),
            # then average those minimal contributions over all test cases and count solved ones.
            best_par2_seconds = None
            best_solved_count = None
            if all_cases:
                timeout_ms = timeout_seconds * 1000.0
                par2_penalty_ms = 2 * timeout_ms
                best_total_ms = 0.0
                solved_best_total = 0
                for case in all_cases:
                    best_contrib = par2_penalty_ms  # Initialize with penalty
                    for m in seed_maps:
                        if case not in m:
                            continue
                        entry = m[case]
                        try:
                            sim_ms = float(entry.get('sim_time_ms', 0.0) or 0.0)
                        except (TypeError, ValueError):
                            sim_ms = 0.0
                        if entry.get('result') in ('SAT', 'UNSAT') and sim_ms <= timeout_ms:
                            contrib = sim_ms
                        else:
                            contrib = par2_penalty_ms
                        if contrib < best_contrib:
                            best_contrib = contrib
                    best_total_ms += best_contrib
                    if best_contrib < par2_penalty_ms:
                        solved_best_total += 1
                best_par2_seconds = (best_total_ms / len(all_cases)) / 1000.0
                best_solved_count = solved_best_total
                print(f"Best-of-seeds -> Solved: {best_solved_count}/{len(all_cases)}, PAR-2: {best_par2_seconds:.2f} s (timeout: {timeout_seconds}s)")

            # Compute max simulated time across all seeds/examples. For unfinished (UNKNOWN/ERROR/UNKNOWN N) results,
            # use last stats CSV dump time as fallback. We look inside each seed directory for <test_case>.stats.csv.
            max_sim_ms = 0.0
            for si, res_list in enumerate(seed_results):
                seed_dir = seed_dirs[si]
                for r in res_list:
                    sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
                    label = r.get('result') or ''
                    if not (label.startswith('SAT') or label.startswith('UNSAT')):  # includes UNKNOWN variants and ERROR
                        fallback_ms = _get_last_stats_time_ms(seed_dir, r.get('test_case', ''))
                        sim_ms = max(sim_ms, fallback_ms)
                    if sim_ms > max_sim_ms:
                        max_sim_ms = sim_ms
            print(f"Max simulated time across all seeds: {max_sim_ms:.2f} ms")

            # Generate output: aggregated results by default; best-of-seeds if requested
            if output_file:
                if dump_best and all_cases:
                    # Build best-of-seeds result rows by selecting the seed entry with minimal PAR-2 contribution per test
                    timeout_ms = timeout_seconds * 1000.0
                    par2_penalty_ms = 2 * timeout_ms
                    best_results = []
                    for case in sorted(all_cases):
                        best_entry = None
                        best_contrib = par2_penalty_ms
                        for m in seed_maps:
                            if case not in m:
                                continue
                            entry = m[case]
                            try:
                                sim_ms = float(entry.get('sim_time_ms', 0.0) or 0.0)
                            except (TypeError, ValueError):
                                sim_ms = 0.0
                            if entry.get('result') in ('SAT', 'UNSAT') and sim_ms <= timeout_ms:
                                contrib = sim_ms
                            else:
                                contrib = par2_penalty_ms
                            if contrib < best_contrib:
                                best_contrib = contrib
                                best_entry = entry
                        if best_entry is None:
                            # Synthesize a placeholder UNKNOWN row so timeouts/missing logs are represented
                            row = {
                                'test_case': case,
                                'result': 'UNKNOWN',
                                'sim_time_ms': par2_penalty_ms,
                                'total_memory_bytes': 0,
                            }
                        else:
                            row = dict(best_entry)
                        if best_contrib >= par2_penalty_ms:
                            row['result'] = 'UNKNOWN'
                            row['sim_time_ms'] = par2_penalty_ms
                        else:
                            try:
                                row['sim_time_ms'] = float(row.get('sim_time_ms', 0.0) or 0.0)
                            except (TypeError, ValueError):
                                row['sim_time_ms'] = 0.0
                        row['total_memory_formatted'] = format_bytes(int(row.get('total_memory_bytes', 0) or 0))
                        best_results.append(row)

                    write_csv_report(
                        best_results,
                        output_file,
                        par2_score_seconds=(best_par2_seconds or 0.0),
                        solved_count=(best_solved_count or 0),
                        total_problems=len(all_cases),
                    )
                    print(f"CSV report (best-of-seeds) written to: {output_file}")
                else:
                    write_csv_report(
                        aggregated_results,
                        output_file,
                        par2_score_seconds=avg_par2_seconds,
                        solved_count=avg_solved,
                        total_problems=avg_num_cases,
                    )
                    print(f"CSV report written to: {output_file}")

            # In multi-seed mode, we've already printed a clear per-seed and
            # averaged summary and (optionally) written the CSV. Avoid falling
            # through to the single-run statistics summary, which can be
            # confusing because it operates on aggregated entries with
            # unanimity-based SAT/UNSAT labels. Return early here.
            return
        else:
            # No valid seed logs, fall back to parsing at top-level
            results = parse_log_directory(folder_path, exclude_summary=True)
            if not results:
                print(f"No valid log files found in {folder_path}")
                return
    else:
        # Single-run folder: parse logs in the folder directly
        results = parse_log_directory(folder_path, exclude_summary=True)
        
        if not results:
            print(f"No valid log files found in {folder_path}")
            return

        # Apply timeout classification directly to per-test rows
        timeout_ms = timeout_seconds * 1000.0
        par2_penalty_ms = 2 * timeout_ms
        for r in results:
            try:
                sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
            except (TypeError, ValueError):
                sim_ms = 0.0
            if r.get('result') in ('SAT','UNSAT') and sim_ms > timeout_ms:
                r['result'] = 'UNKNOWN'
            # For any UNKNOWN (including timeouts or parse-unknown), set sim_time_ms to PAR-2 penalty
            if r.get('result') not in ('SAT','UNSAT'):
                r['sim_time_ms'] = par2_penalty_ms

    # unified_parser already enriches each result with CSV prefetch req/drops
    
    # Print parsing summary
    print(f"Successfully parsed: {len(results)} files")
    
    # Sort results by test case name
    results.sort(key=lambda x: x['test_case'])
    
    # Statistics
    sat_count = sum(1 for r in results if r.get('result') == 'SAT')
    unsat_count = sum(1 for r in results if r.get('result') == 'UNSAT')
    unknown_count = sum(1 for r in results if r.get('result') == 'UNKNOWN')
    solved_count = sat_count + unsat_count  # raw solved by label
    
    total_memory = sum(r['total_memory_bytes'] for r in results)
    avg_memory = total_memory / len(results) if results else 0
    total_decisions = sum(r.get('decisions', 0) for r in results)
    avg_decisions = total_decisions / len(results) if results else 0
    
    # Calculate PAR-2 score
    # For solved instances: use actual time
    # For unsolved instances: use 2 * timeout
    timeout_ms = timeout_seconds * 1000.0
    par2_penalty = 2 * timeout_ms
    
    par2_total = 0.0
    solved_within_timeout = 0
    for r in results:
        sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
        if r['result'] in ['SAT', 'UNSAT'] and sim_ms <= timeout_ms:
            # Solved within timeout: use actual time
            par2_total += sim_ms
            solved_within_timeout += 1
        else:
            # Unknown or exceeded timeout: use 2*timeout penalty
            par2_total += par2_penalty
    
    par2_score = par2_total / len(results) if results else 0.0
    par2_score /= 1000.0  # Convert to seconds for reporting

    # Generate output (after stats so we can include a final summary row)
    if output_file and not seed_dirs:
        # Only write here for single-run mode. Multi-seed writes earlier.
        write_csv_report(
            results,
            output_file,
            par2_score_seconds=par2_score,
            solved_count=solved_within_timeout,
            total_problems=len(results),
        )
        print(f"CSV report written to: {output_file}")
    
    # Detect log type by checking for satsolver-specific fields
    l1_results = [r for r in results if r.get('l1_total_requests', 0) > 0]
    is_satsolver_logs = len(l1_results) > 0
    
    print(f"\n=== STATISTICS SUMMARY ===")
    print(f"Total problems: {len(results)} (SAT: {sat_count}, UNSAT: {unsat_count}, UNKNOWN: {unknown_count})\n")
    print(f"Solved: {solved_within_timeout}/{len(results)} ({100.0 * solved_within_timeout / len(results) if results else 0:.1f}%)")
    print(f"PAR-2 score: {par2_score:.2f} s (timeout: {timeout_seconds}s)\n")
    
    # Runtime statistics
    if results:
        # Max simulated time across problems. For unfinished tests, use last
        # statistics dump time from the corresponding <test_case>.stats.csv
        max_sim_ms = 0.0
        for r in results:
            sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
            if r.get('result') not in ('SAT', 'UNSAT'):
                fallback_ms = _get_last_stats_time_ms(folder_path, r.get('test_case', ''))
                # Prefer whichever is larger as best proxy for simulated time reached
                sim_ms = max(sim_ms, fallback_ms)
            if sim_ms > max_sim_ms:
                max_sim_ms = sim_ms
        print(f"Max simulated time: {max_sim_ms:.2f} ms")

        total_time = sum(r.get('sim_time_ms', 0) for r in results)
        avg_time = total_time / len(results)
        print(f"Average runtime per problem: {avg_time:.2f} ms")
    
    # Solver statistics
    print(f"Average memory per problem: {format_bytes(avg_memory)}")
    print()
    print(f"Average decisions per problem: {avg_decisions:.1f}")
    
    if results:
        total_propagations = sum(r.get('propagations', 0) for r in results)
        avg_propagations = total_propagations / len(results)
        total_conflicts = sum(r.get('conflicts', 0) for r in results)
        avg_conflicts = total_conflicts / len(results)
        total_learned = sum(r.get('learned', 0) for r in results)
        avg_learned = total_learned / len(results)
        total_restarts = sum(r.get('restarts', 0) for r in results)
        avg_restarts = total_restarts / len(results)
        
        print(f"Average propagations per problem: {avg_propagations:.1f}")
        print(f"Average conflicts per problem: {avg_conflicts:.1f}")
        print(f"Average learned clauses per problem: {avg_learned:.1f}")
        print(f"Average restarts per problem: {avg_restarts:.1f}")
    
    # Satsolver-specific statistics (L1 cache, cycles, etc.)
    if l1_results:
        avg_l1_miss_rate = sum(r.get('l1_total_miss_rate', 0) for r in l1_results) / len(l1_results)
        
        # Calculate average miss rates per data structure
        components = ['heap', 'variables', 'watches', 'clauses', 'varactivity']
        component_stats = {}
        for comp in components:
            comp_results = [r for r in l1_results if r.get(f'l1_{comp}_total', 0) > 0]
            if comp_results:
                avg_miss_rate = sum(r.get(f'l1_{comp}_miss_rate', 0) for r in comp_results) / len(comp_results)
                component_stats[comp] = avg_miss_rate
        
        print(f"\nProblems with L1 cache data: {len(l1_results)}")
        print(f"Average L1 miss rate: {avg_l1_miss_rate:.2f}%")
        print("Average miss rates by data structure:")
        for comp, miss_rate in component_stats.items():
            print(f"  {comp.capitalize()}: {miss_rate:.2f}%")

    # Prefetch stats summary (DirectedPrefetcher + CSV requests/drops)
    prefetch_results = [r for r in results if any(k in r for k in ('prefetches_issued','prefetches_used','prefetches_unused','prefetch_accuracy','l1_prefetch_requests','l1_prefetch_drops'))]
    if prefetch_results:
        avg_acc = sum(r.get('prefetch_accuracy', 0.0) for r in prefetch_results if r.get('prefetch_accuracy') is not None) / max(1, sum(1 for r in prefetch_results if 'prefetch_accuracy' in r))
        avg_requests = sum(r.get('l1_prefetch_requests', 0) for r in prefetch_results) / len(prefetch_results)
        avg_drops = sum(r.get('l1_prefetch_drops', 0) for r in prefetch_results) / len(prefetch_results)
        print(f"\nPrefetch stats across problems: {len(prefetch_results)} with data")
        print(f"Average Prefetch accuracy: {avg_acc:.2f}% (if present)")
        print(f"Average L1 Prefetch requests (CSV): {avg_requests:.1f}")
        print(f"Average L1 Prefetch drops (CSV): {avg_drops:.1f}")

    # Propagation detail presence
    prop_keys = sorted({k for r in results for k in r if k.startswith('prop_')})
    if prop_keys:
        print(f"\nPropagation detail statistics collected for {sum(1 for r in results if any(k in r for k in prop_keys))} problems.")


def main():
    if len(sys.argv) < 2:
        print("Usage: python parse_results.py <results_folder> [output_file] [--timeout SECONDS] [--dump-best]")
        print("Example: python parse_results.py ../runs/logs results.csv")
        print("Example: python parse_results.py ../runs/logs results.csv --timeout 3600 --dump-best")
        sys.exit(1)
    
    results_folder = sys.argv[1]
    output_file = None
    timeout_seconds = 3600  # Default timeout
    dump_best = False
    
    # Parse arguments
    i = 2
    while i < len(sys.argv):
        if sys.argv[i] == '--timeout' and i + 1 < len(sys.argv):
            timeout_seconds = float(sys.argv[i + 1])
            i += 2
        elif sys.argv[i] == '--dump-best':
            dump_best = True
            i += 1
        else:
            # Assume it's the output file if not a flag
            if output_file is None and not sys.argv[i].startswith('--'):
                output_file = sys.argv[i]
            i += 1

    parse_results_folder(results_folder, output_file, timeout_seconds, dump_best)


if __name__ == "__main__":
    main()
