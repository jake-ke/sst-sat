#!/usr/bin/env python3
"""
SAT Solver Multi-Folder Comparison Plotter

This script compares results from multiple input folders or raw text files and generates comparison charts.
Supports both log directories (parsed with unified_parser) and raw CSV/text files with columns:
  - name/test/test_case: test case name
  - par2_ms/par-2/sim_time_ms: timing data in milliseconds

Usage: python plot_comparison.py <folder1|file1> <folder2|file2> [...] [--timeout SECONDS] [--output-dir DIR]

Examples:
    python plot_comparison.py runs/baseline runs/optimized --output-dir results/
    python plot_comparison.py logs_4MiB logs_8MiB logs_16MiB --timeout 36
    python plot_comparison.py results1.txt results2.txt --names "Config A" "Config B"
"""

import sys
import csv
import argparse
from pathlib import Path
from collections import defaultdict
import math
import matplotlib.pyplot as plt
import matplotlib.backends.backend_pdf

# Unified figure size for all charts
FIG_SIZE = (10, 6)
from unified_parser import parse_log_directory, format_bytes


# Manual exclusion list: add test case names here to exclude them from all comparisons
MANUAL_EXCLUSIONS = set([
    # "080896c437245ac25eb6d3ad6df12c4f-bv-term-small-rw_1492.smt2.cnf",
    # "e17d3f94f2c0e11ce6143bc4bf298bd7-mp1-qpr-bmp280-driver-5.cnf",
    # "e185ebbd92c23ab460a3d29046eccf1d-group_mulr.cnf",
])

# Manual exclusive test set: if non-empty, ONLY these tests are considered for
# comparison logic (subject still to MANUAL_EXCLUSIONS removal). Any test listed
# here but missing/ERROR/UNKNOWN in a folder will appear in the exclusion table.
MANUAL_EXCLUSIVE_TESTS = set([
    # >120k variables or >640k clauses
    # "06e928088bd822602edb83e41ce8dadb-satcoin-genesis-SAT-10.cnf",
    # "43e492bccfd57029b758897b17d7f04f-pb_300_09_lb_07.cnf",
    # "aacfb8797097f698d14337d3a04f3065-barman-pfile06-022.sas.ex.7.cnf",
    # "ff6b6ad55c0dffe034bc8daa9129ff1d-satcoin-genesis-SAT-10-sc2018.cnf",

    # > 1m clauses
    # "e185ebbd92c23ab460a3d29046eccf1d-group_mulr.cnf",  # did not finish
    # "91c69a1dedfa5f2b3779e38c48356593-Problem14_label20_true-unreach-call.c.cnf",
    # "7b5895f110a6aa5a5ac5d5a6eb3fd322-g2-ak128modasbg1sbisc.cnf",
    # "7aa3b29dde431cdacf17b2fb9a10afe4-Mario-t-hard-2_c18.cnf",
    # "fce130da1efd25fa9b12e59620a4146b-g2-ak128diagobg1btaig.cnf",
    # "1427381a809c64c721838894ece6756d-shuffling-2-s25242449-of-bench-sat04-727.used-as.sat04-753.cnf",
])

def normalize_test_case(name: str) -> str:
    """Normalize test case name by stripping common SAT/SMT file extensions.
    This enables matching raw names (without extension) to log-derived names.
    """
    if not name:
        return name
    # Known extensions (lowercase)
    known_exts = {'.cnf', '.dimacs', '.smt2', '.xml', '.c', '.txt', '.log'}
    lower = name.lower()
    for ext in known_exts:
        if lower.endswith(ext):
            return name[:-(len(ext))]
    return name


def parse_raw_text_file(file_path, timeout_seconds):
    """Parse a raw text file with columns: name, sim_time_ms, par2_ms (or similar).
    
    Returns:
        list of result dicts with keys: test_case, result, sim_time_ms, etc.
    """
    results = []
    timeout_ms = timeout_seconds * 1000.0
    
    with open(file_path, 'r') as f:
        lines = [line.strip() for line in f if line.strip()]
    
    if not lines:
        return []
    
    # Parse header
    header = lines[0].lower()
    cols = [c.strip() for c in header.split(',')]
    
    # Find relevant columns
    name_col = None
    par2_col = None
    sim_time_col = None
    
    for i, col in enumerate(cols):
        if col in ('name', 'test', 'testcase', 'test_case', 'benchmark'):
            name_col = i
        elif 'par2' in col or 'par-2' in col:
            par2_col = i
        elif 'sim_time' in col or 'time' in col:
            sim_time_col = i
    
    if name_col is None:
        print(f"Warning: Could not find name column in {file_path}")
        return []
    
    if par2_col is None and sim_time_col is None:
        print(f"Warning: Could not find time column in {file_path}")
        return []
    
    # Use par2 column if available, otherwise sim_time
    time_col = par2_col if par2_col is not None else sim_time_col
    
    # Parse data rows
    for line in lines[1:]:
        parts = [p.strip() for p in line.split(',')]
        if len(parts) <= max(name_col, time_col):
            continue
        
        test_case = parts[name_col]
        try:
            time_ms = float(parts[time_col])
        except (ValueError, IndexError):
            continue
        
        # Determine result based on time
        if time_ms >= 2 * timeout_ms:
            result = 'TIMEOUT'
        elif time_ms > timeout_ms:
            result = 'TIMEOUT'
        else:
            # Assume solved if within timeout
            result = 'SAT'  # We don't know if SAT or UNSAT from raw data
        
        results.append({
            'original_test_case': test_case,
            'test_case': test_case,  # this specific raw text format does not have extensions
            'result': result,
            'sim_time_ms': time_ms,
            'total_memory_bytes': 0,
            'variables': 0,
            'clauses': 0,
        })
    
    return results


def is_raw_text_file(path):
    """Check if path is a raw text file (not a directory)."""
    p = Path(path)
    return p.is_file() and p.suffix in ('.txt', '.csv', '.tsv', '')


def compute_metrics_for_folder(folder_path, timeout_seconds):
    """Parse a folder or raw text file and compute key metrics.
    
    Returns:
        dict with keys:
        - 'results': list of parsed result dicts
        - 'par2_score': PAR-2 score in seconds (or None if no valid results)
        - 'solved_count': number solved within timeout
        - 'total_count': total valid problems (excludes ERROR/UNKNOWN)
        - 'excluded_tests': list of test cases with ERROR/UNKNOWN
    """
    folder_path = Path(folder_path)
    
    # Check if it's a raw text file
    if is_raw_text_file(folder_path):
        results = parse_raw_text_file(folder_path, timeout_seconds)
        
        if not results:
            return {
                'results': [],
                'par2_score': None,
                'solved_count': 0,
                'total_count': 0,
                'excluded_tests': []
            }
        
        # Calculate metrics
        timeout_ms = timeout_seconds * 1000.0
        par2_penalty = 2 * timeout_ms
        
        par2_total = 0.0
        solved_within_timeout = 0
        valid_results = [r for r in results if r.get('result') not in ('ERROR', 'UNKNOWN')]
        
        for r in valid_results:
            if r['result'] == 'TIMEOUT':
                par2_total += par2_penalty
            else:
                sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
                if sim_ms <= timeout_ms:
                    par2_total += sim_ms
                    solved_within_timeout += 1
                else:
                    par2_total += par2_penalty
        
        par2_score = (par2_total / len(valid_results)) / 1000.0 if valid_results else None
        
        return {
            'results': results,
            'par2_score': par2_score,
            'solved_count': solved_within_timeout,
            'total_count': len(valid_results),
            'excluded_tests': []
        }
    
    # Otherwise, treat as directory and use unified_parser
    # Check for multi-seed layout
    seed_dirs = [p for p in sorted(folder_path.glob('seed*')) if p.is_dir()]
    
    if seed_dirs:
        # Multi-seed: parse each seed and aggregate by averaging
        seed_results = []
        for sd in seed_dirs:
            res = parse_log_directory(sd, exclude_summary=True)
            if res:
                seed_results.append(res)
        
        if not seed_results:
            return {
                'results': [],
                'par2_score': None,
                'solved_count': 0,
                'total_count': 0,
                'excluded_tests': []
            }
        
        # Build test_case union
        all_cases = set()
        for res in seed_results:
            for r in res:
                all_cases.add(r.get('test_case', ''))
        
        # Map seed index -> {test_case -> result}
        seed_maps = []
        for res in seed_results:
            m = {r.get('test_case', ''): r for r in res}
            seed_maps.append(m)
        
        # Aggregate per test_case by averaging
        timeout_ms = timeout_seconds * 1000.0
        par2_penalty_ms = 2 * timeout_ms
        
        def _avg_of(values):
            vals = [v for v in values if v is not None]
            if not vals:
                return 0
            return sum(vals) / len(vals)
        
        numeric_fields = [
            'variables', 'clauses', 'total_memory_bytes', 'sim_time_ms',
            'decisions', 'propagations', 'conflicts', 'learned', 'removed',
            'db_reductions', 'minimized', 'restarts',
            'l1_total_requests', 'l1_total_miss_rate'
        ]
        
        aggregated_results = []
        for case in sorted(all_cases):
            entries = [m[case] for m in seed_maps if case in m]
            if not entries:
                continue
            
            agg = {'original_test_case': case, 'test_case': normalize_test_case(case)}
            
            # Determine aggregate result label
            seed_labels = [e.get('result') for e in entries]
            unique_labels = set(seed_labels)
            
            if len(unique_labels) == 1:
                agg['result'] = seed_labels[0]
            else:
                # Mixed results
                has_sat = 'SAT' in unique_labels
                has_unsat = 'UNSAT' in unique_labels
                
                if has_sat and has_unsat:
                    count = len([l for l in seed_labels if l != 'SAT'])
                    agg['result'] = f"SAT {count}/{len(seed_labels)}"
                elif has_sat:
                    count = len([l for l in seed_labels if l != 'SAT'])
                    agg['result'] = f"SAT {count}/{len(seed_labels)}"
                elif has_unsat:
                    count = len([l for l in seed_labels if l != 'UNSAT'])
                    agg['result'] = f"UNSAT {count}/{len(seed_labels)}"
                else:
                    majority = max(set(seed_labels), key=seed_labels.count)
                    count = len([l for l in seed_labels if l != majority])
                    agg['result'] = f"{majority} {count}/{len(seed_labels)}"
            
            # Check if abnormal or mixed non-timeout
            result_str = agg.get('result', '')
            primary_label = result_str.split()[0] if result_str else ''
            is_abnormal = primary_label in ('ERROR', 'UNKNOWN')
            is_mixed_non_timeout = (' ' in result_str) and (primary_label != 'TIMEOUT')
            
            if is_abnormal or is_mixed_non_timeout:
                aggregated_results.append(agg)
                continue
            
            # Average numeric fields
            finished = []
            for e in entries:
                if e.get('result') in ('SAT', 'UNSAT'):
                    sim_ms = float(e.get('sim_time_ms', 0.0) or 0.0)
                    if sim_ms <= timeout_ms:
                        finished.append(e)
            
            entries_for_averaging = entries if result_str == 'TIMEOUT' or result_str.startswith('TIMEOUT ') else finished
            
            for key in numeric_fields:
                if key == 'sim_time_ms':
                    continue
                vals = []
                for e in entries_for_averaging:
                    v = e.get(key)
                    if v is not None:
                        try:
                            vals.append(float(v))
                        except (TypeError, ValueError):
                            pass
                agg[key] = _avg_of(vals)
            
            # sim_time_ms uses PAR-2 semantics
            time_vals = []
            for e in entries:
                if e.get('result') in ('ERROR', 'UNKNOWN'):
                    continue
                if e.get('result') == 'TIMEOUT':
                    time_vals.append(par2_penalty_ms)
                else:
                    sim_ms = float(e.get('sim_time_ms', 0.0) or 0.0)
                    if e.get('result') in ('SAT', 'UNSAT') and sim_ms <= timeout_ms:
                        time_vals.append(sim_ms)
                    else:
                        time_vals.append(par2_penalty_ms)
            agg['sim_time_ms'] = _avg_of(time_vals)
            agg['total_memory_formatted'] = format_bytes(int(agg.get('total_memory_bytes', 0) or 0))
            aggregated_results.append(agg)
        
        results = aggregated_results
    else:
        # Single-run folder
        results = parse_log_directory(folder_path, exclude_summary=True)
        
        if not results:
            return {
                'results': [],
                'par2_score': None,
                'solved_count': 0,
                'total_count': 0,
                'excluded_tests': []
            }
        
        # Apply timeout classification
        timeout_ms = timeout_seconds * 1000.0
        par2_penalty_ms = 2 * timeout_ms
        for r in results:
            try:
                sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
            except (TypeError, ValueError):
                sim_ms = 0.0
            
            if r.get('result') in ('SAT', 'UNSAT') and sim_ms > timeout_ms:
                r['result'] = 'TIMEOUT'
            
            if r.get('result') == 'TIMEOUT':
                r['sim_time_ms'] = par2_penalty_ms

        # Apply normalization to single-run results
        for r in results:
            tc = r.get('test_case')
            if tc is not None:
                r['original_test_case'] = tc
                r['test_case'] = normalize_test_case(tc)
    
    # Collect excluded tests
    excluded_tests = [r['test_case'] for r in results if r.get('result') in ('ERROR', 'UNKNOWN')]
    
    # Calculate PAR-2 score
    timeout_ms = timeout_seconds * 1000.0
    par2_penalty = 2 * timeout_ms
    
    par2_total = 0.0
    solved_within_timeout = 0
    valid_results = [r for r in results if r.get('result') not in ('ERROR', 'UNKNOWN')]
    
    for r in valid_results:
        if r['result'] == 'TIMEOUT':
            par2_total += par2_penalty
        else:
            sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
            if r['result'] in ['SAT', 'UNSAT'] and sim_ms <= timeout_ms:
                par2_total += sim_ms
                solved_within_timeout += 1
            else:
                par2_total += par2_penalty
    
    par2_score = (par2_total / len(valid_results)) / 1000.0 if valid_results else None
    
    return {
        'results': results,
        'par2_score': par2_score,
        'solved_count': solved_within_timeout,
        'total_count': len(valid_results),
        'excluded_tests': excluded_tests
    }


def get_shared_test_set(folder_metrics, timeout_seconds=None, exclude_timeouts=False):
    """Determine the shared set of tests that finished (not ERROR/UNKNOWN) in all folders.
    
    Args:
        folder_metrics: dict of folder results
        timeout_seconds: timeout value in seconds (required if exclude_timeouts=True)
        exclude_timeouts: if True, also exclude TIMEOUT tests from shared set
    
    Returns:
        tuple of (shared_tests: set, exclusion_table: list of tuples)
        exclusion_table contains (test_case, folder_name, reason) for excluded tests
    """
    if not folder_metrics:
        return set(), []
    
    timeout_ms = timeout_seconds * 1000.0 if timeout_seconds else None
    
    # Build per-folder test status maps and timing info
    folder_test_info = {}
    for folder_name, metrics in folder_metrics.items():
        info_map = {}
        for r in metrics['results']:
            test_case = r.get('test_case', '')
            result = r.get('result', 'UNKNOWN')
            primary_label = result.split()[0] if result else 'UNKNOWN'
            try:
                sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
            except (TypeError, ValueError):
                sim_ms = 0.0
            info_map[test_case] = (primary_label, sim_ms)
        folder_test_info[folder_name] = info_map
    
    # Find all test cases present in any folder
    all_tests = set()
    for info_map in folder_test_info.values():
        all_tests.update(info_map.keys())

    # Normalize manual exclusions and exclusives for matching
    normalized_exclusions = {normalize_test_case(n) for n in MANUAL_EXCLUSIONS}
    normalized_exclusive = {normalize_test_case(n) for n in MANUAL_EXCLUSIVE_TESTS}

    if normalized_exclusive:
        # Use the normalized exclusive set as candidate tests (include even if missing to record exclusion)
        candidate_tests = sorted(normalized_exclusive)
    else:
        candidate_tests = sorted(all_tests)
    
    # Determine shared tests (finished in ALL folders, optionally excluding timeouts)
    shared_tests = set()
    exclusion_table = []
    
    for test_case in candidate_tests:
        # Check manual exclusion list first (normalized)
        if test_case in normalized_exclusions:
            exclusion_table.append((test_case, 'MANUAL', 'MANUAL_EXCLUSION'))
            continue
        
        is_valid_in_all = True
        excluding_folder = None
        excluding_reason = None
        
        for folder_name in sorted(folder_metrics.keys()):
            info_map = folder_test_info[folder_name]
            if test_case not in info_map:
                is_valid_in_all = False
                excluding_folder = folder_name
                excluding_reason = 'MISSING'
                break
            
            status, sim_ms = info_map[test_case]
            
            # Always exclude ERROR/UNKNOWN
            if status in ('ERROR', 'UNKNOWN'):
                is_valid_in_all = False
                excluding_folder = folder_name
                excluding_reason = status
                break
            
            # Optionally exclude TIMEOUT
            if exclude_timeouts and timeout_ms is not None:
                if status == 'TIMEOUT' or (status in ('SAT', 'UNSAT') and sim_ms > timeout_ms):
                    is_valid_in_all = False
                    excluding_folder = folder_name
                    excluding_reason = 'TIMEOUT'
                    break
        
        if is_valid_in_all:
            shared_tests.add(test_case)
        else:
            exclusion_table.append((test_case, excluding_folder, excluding_reason))
    
    return shared_tests, exclusion_table


def compute_par2_on_shared_set(folder_metrics, shared_tests, timeout_seconds):
    """Compute PAR-2 scores using only the shared test set.
    
    Returns:
        dict mapping folder_name -> (par2_score, solved_count, total_count)
    """
    timeout_ms = timeout_seconds * 1000.0
    par2_penalty = 2 * timeout_ms
    
    par2_scores = {}
    
    for folder_name, metrics in folder_metrics.items():
        # Filter to shared tests only
        shared_results = [r for r in metrics['results'] 
                         if r.get('test_case') in shared_tests 
                         and r.get('result') not in ('ERROR', 'UNKNOWN')]
        
        if not shared_results:
            par2_scores[folder_name] = (None, 0, 0)
            continue
        
        par2_total = 0.0
        solved = 0
        
        for r in shared_results:
            if r['result'] == 'TIMEOUT':
                par2_total += par2_penalty
            else:
                sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
                if r['result'] in ['SAT', 'UNSAT'] and sim_ms <= timeout_ms:
                    par2_total += sim_ms
                    solved += 1
                else:
                    par2_total += par2_penalty
        
        par2_score = (par2_total / len(shared_results)) / 1000.0
        par2_scores[folder_name] = (par2_score, solved, len(shared_results))
    
    return par2_scores


def compute_geomean_speedups(folder_metrics, shared_tests, timeout_seconds, baseline_name):
    """Compute geometric mean speedup for each folder vs baseline using the shared test set.

    Speedup per test = t_baseline / t_config with times in milliseconds.
    - Uses PAR-2 effective times: timeouts or >timeout get 2*timeout penalty.
    - Computes over the provided shared_tests set (which may already have timeouts excluded).

    Returns: dict mapping folder_name -> geomean_speedup (float or None).
    Baseline has speedup 1.0.
    """
    timeout_ms = timeout_seconds * 1000.0
    penalty_ms = 2 * timeout_ms

    # Build mapping folder -> test_case -> (result, sim_ms)
    folder_case_map = {}
    for folder_name, metrics in folder_metrics.items():
        case_map = {}
        for r in metrics['results']:
            tc = r.get('test_case')
            if tc not in shared_tests:
                continue
            try:
                sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
            except (TypeError, ValueError):
                sim_ms = penalty_ms
            case_map[tc] = (r.get('result'), sim_ms)
        folder_case_map[folder_name] = case_map

    geomeans = {}
    baseline_map = folder_case_map.get(baseline_name, {})

    for folder_name in folder_metrics.keys():
        if folder_name == baseline_name:
            geomeans[folder_name] = 1.0
            continue

        ln_sum = 0.0
        n = 0
        case_map = folder_case_map.get(folder_name, {})
        
        # PAR-2 over all shared tests
        for tc in shared_tests:
            b = baseline_map.get(tc)
            c = case_map.get(tc)
            if not b or not c:
                continue
            b_res, b_ms = b
            c_res, c_ms = c
            tb_eff = b_ms if (b_res in ('SAT', 'UNSAT') and b_ms <= timeout_ms) else penalty_ms
            tc_eff = c_ms if (c_res in ('SAT', 'UNSAT') and c_ms <= timeout_ms) else penalty_ms
            if tc_eff <= 0 or tb_eff <= 0:
                continue
            speedup = tb_eff / tc_eff
            if speedup <= 0:
                continue
            ln_sum += math.log(speedup)
            n += 1
        geomeans[folder_name] = math.exp(ln_sum / n) if n > 0 else None

    return geomeans


def plot_comparison_charts(folder_metrics, shared_par2_scores, shared_tests, 
                          exclusion_table, timeout_seconds, output_dir):
    """Generate all comparison charts and save to PDF."""
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    pdf_path = output_dir / 'comparison_charts.pdf'
    
    with matplotlib.backends.backend_pdf.PdfPages(pdf_path) as pdf:
        # Chart 1: PAR-2 Score Comparison (shared set)
        fig, ax = plt.subplots(figsize=FIG_SIZE)
        
        folder_names = list(folder_metrics.keys())
        par2_values = [shared_par2_scores[name][0] for name in folder_names]
        solved_counts = [shared_par2_scores[name][1] for name in folder_names]
        total_count = shared_par2_scores[folder_names[0]][2] if folder_names else 0
        # Grouped (clustered) bar chart with a single implicit category.
        # We suppress x-axis tick labels; legend conveys configuration names.
        x_positions = [i for i in range(len(folder_names))]
        bars = ax.bar(x_positions, par2_values, color=plt.cm.tab20.colors[:len(folder_names)], alpha=0.85, edgecolor='black', linewidth=0.8)
        ax.set_ylabel('PAR-2 (s)', fontsize=26)
        ax.set_xticks([])  # no x-axis categories shown
        ax.tick_params(axis='y', labelsize=22)
        ax.grid(axis='y', alpha=0.3)
        # Boxed legend with folder names
        ax.legend(bars, folder_names, loc='upper right', fontsize=18, frameon=True)
        
        # Set y-axis limit to provide more space for labels
        max_par2 = max(par2_values) if par2_values else 1
        ax.set_ylim(0, max_par2 * 1.2)
        
        # Add value labels on bars (PAR-2 and solved count side by side)
        for i, (bar, par2, solved) in enumerate(zip(bars, par2_values, solved_counts)):
            height = bar.get_height()
            ax.text(bar.get_x() + bar.get_width()/2., height,
                    f'{par2:.2f} s\nSolved: {solved}',
                    ha='center', va='bottom', fontsize=18)
        
        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)

        # Geomean speedup chart moved to separate PDF; no longer included here.
    
    print(f"\nPAR-2 chart saved to: {pdf_path}")


def plot_cactus_chart(folder_metrics, shared_tests, timeout_seconds, output_dir, pdf_basename='cactus_plot'):
    """Generate a cactus (survival) plot: cumulative solved instances vs time for each configuration.
    Each line: sorted solve times (ms->s) of SAT/UNSAT results within timeout on the shared test set.
    Saved as a separate PDF.
    """
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    pdf_path = output_dir / f'{pdf_basename}.pdf'

    timeout_ms = timeout_seconds * 1000.0

    # Collect solved times per configuration
    folder_names = list(folder_metrics.keys())
    solved_time_map = {}
    for folder_name in folder_names:
        results = folder_metrics[folder_name]['results']
        times = []
        for r in results:
            if r.get('test_case') not in shared_tests:
                continue
            if r.get('result') not in ('SAT', 'UNSAT'):
                continue
            try:
                sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
            except (TypeError, ValueError):
                continue
            if sim_ms <= timeout_ms:
                times.append(sim_ms)
        if times:
            solved_time_map[folder_name] = sorted(times)
        else:
            solved_time_map[folder_name] = []

    if not any(solved_time_map.values()):
        print("No solved instances within timeout for cactus plot; skipping PDF generation.")
        return

    with matplotlib.backends.backend_pdf.PdfPages(pdf_path) as pdf:
        fig, ax = plt.subplots(figsize=FIG_SIZE)

        color_cycle = plt.cm.tab20.colors
        for idx, folder_name in enumerate(folder_names):
            times = solved_time_map.get(folder_name, [])
            if not times:
                continue
            # Sort times already; build cumulative solved vs wallclock time curve.
            # We plot step-like cumulative vs time by connecting points.
            sorted_secs = [t / 1000.0 for t in times]
            x_vals = sorted_secs
            y_vals = list(range(1, len(sorted_secs) + 1))
            ax.plot(x_vals, y_vals, linestyle=':', marker='o', markersize=4,
                    color=color_cycle[idx % len(color_cycle)], label=folder_name)

        ax.set_xlabel('Wallclock Time (s)', fontsize=26)
        ax.set_ylabel('Solved Instances', fontsize=26)
        ax.tick_params(axis='both', labelsize=22)
        ax.grid(alpha=0.3, linestyle='--')
        # Legend without title, boxed
        ax.legend(loc='lower right', fontsize=18, frameon=True)
        # No plot title per request.

        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)

    print(f"Cactus plot saved to: {pdf_path}")


def plot_geomean_chart(folder_metrics, shared_tests, timeout_seconds, output_dir, baseline_name, geomean_results, pdf_basename='geomean_speedups'):
    """Plot geomean speedup bar chart in a separate PDF.
    geomean_results: dict folder_name -> geomean_speedup (float or None)
    """
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    pdf_path = output_dir / f'{pdf_basename}.pdf'

    folder_names = list(folder_metrics.keys())
    g_values = []
    for name in folder_names:
        speed = geomean_results.get(name, None)
        if name == baseline_name:
            speed = 1.0
        g_values.append(speed)

    plot_vals = [v if v is not None else 0.0 for v in g_values]
    with matplotlib.backends.backend_pdf.PdfPages(pdf_path) as pdf:
        fig, ax = plt.subplots(figsize=FIG_SIZE)
        x_positions = list(range(len(folder_names)))
        bars = ax.bar(x_positions, plot_vals, color=plt.cm.tab20.colors[:len(folder_names)], alpha=0.85, edgecolor='black', linewidth=0.8)
        ax.set_ylabel('Geomean speedup', fontsize=26)
        ax.set_xticks([])
        ax.tick_params(axis='y', labelsize=22)
        ax.grid(axis='y', alpha=0.3)
        ax.legend(bars, folder_names, loc='upper left', fontsize=18, frameon=True)
        ymax = max(plot_vals) if plot_vals else 1.0
        ax.set_ylim(0, ymax * 1.2)
        for bar, val in zip(bars, g_values):
            height = bar.get_height()
            label = 'n/a' if val is None else f'{val:.2f}×'
            ax.text(bar.get_x() + bar.get_width()/2., height,
                    label, ha='center', va='bottom', fontsize=18)
        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)
    print(f"Geomean speedup chart saved to: {pdf_path}")


def main():
    parser = argparse.ArgumentParser(
        description='Compare SAT solver results from multiple folders or raw text files',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s runs/baseline runs/optimized
  %(prog)s logs_4MiB logs_8MiB logs_16MiB --timeout 36 --output-dir results/
  %(prog)s folder1 folder2 folder3 --names "Config A" "Config B" "Config C"
  %(prog)s results1.txt results2.txt --names "Baseline" "Optimized"
        """
    )
    
    parser.add_argument('folders', nargs='+', help='Input folders or raw text files to compare (in display order)')
    parser.add_argument('--names', nargs='+', 
                       help='Custom names for each folder (must match number of folders)')
    parser.add_argument('--timeout', type=float, default=36, 
                       help='Timeout in seconds for PAR-2 calculation (default: 36)')
    parser.add_argument('--output-dir', default='results', 
                       help='Output directory for plots (default: results/)')
    parser.add_argument('--exclude-timeouts-geomean', action='store_true',
                       help='Exclude TIMEOUT tests from geomean speedup calculation (skip instead of PAR-2 penalty)')
    
    args = parser.parse_args()
    
    if len(args.folders) < 2:
        print("Error: Need at least 2 folders to compare")
        sys.exit(1)
    
    if args.names and len(args.names) != len(args.folders):
        print(f"Error: Number of names ({len(args.names)}) must match number of folders ({len(args.folders)})")
        sys.exit(1)
    
    print(f"Comparing {len(args.folders)} folders with timeout={args.timeout}s")
    print(f"Folders (in order): {', '.join(args.folders)}\n")
    
    # Compute metrics for each folder
    # Use OrderedDict to preserve order and handle duplicate names
    from collections import OrderedDict
    folder_metrics = OrderedDict()
    
    for i, folder_path in enumerate(args.folders):
        # Use custom name if provided, otherwise use folder name
        if args.names:
            folder_name = args.names[i]
        else:
            folder_name = Path(folder_path).name
        
        print(f"Processing {folder_name} ({folder_path})...")
        metrics = compute_metrics_for_folder(folder_path, args.timeout)
        
        if not metrics['results']:
            print(f"  Warning: No valid results found in {folder_path}")
            continue
        
        folder_metrics[folder_name] = metrics
        print(f"  Found {len(metrics['results'])} tests, PAR-2: {metrics['par2_score']:.2f}s, "
              f"Solved: {metrics['solved_count']}/{metrics['total_count']}")
    
    if len(folder_metrics) < 2:
        print("\nError: Need at least 2 folders with valid results")
        sys.exit(1)
    
    # Determine shared test set (with optional timeout exclusion)
    print(f"\nDetermining shared test set (exclude_timeouts={args.exclude_timeouts_geomean})...")
    shared_tests, exclusion_table = get_shared_test_set(folder_metrics, args.timeout, args.exclude_timeouts_geomean)
        
    if exclusion_table:
        print(f"\n=== Excluded Tests ({len(exclusion_table)} tests) ===")
        print(f"{'Excluding Folder':<30} {'Reason':<15} {'Test Case':<100}")
        print("-" * 105)
        for test_case, folder, reason in sorted(exclusion_table):
            print(f"{folder:<30} {reason:<15} {test_case:<100}")

    print(f"Shared test set: {len(shared_tests)} tests")
    print(f"Excluded tests: {len(exclusion_table)}")
    
    if not shared_tests:
        print("Error: No shared tests found across all folders")
        sys.exit(1)

    # Compute PAR-2 on shared set
    shared_par2_scores = compute_par2_on_shared_set(folder_metrics, shared_tests, args.timeout)
    
    # Geomean speedups (baseline = first folder)
    baseline_name = next(iter(folder_metrics.keys()))
    geomean_results = compute_geomean_speedups(folder_metrics, shared_tests, args.timeout, baseline_name)
    
    # Combined table: PAR-2 + Geomean Speedup
    print(f"\n{'Folder':<30} {'PAR-2 (s)':<12} {'Solved/Total':<16} {'Geomean×':<12}")
    print("-" * 74)
    for folder_name in folder_metrics.keys():
        par2, solved, total = shared_par2_scores[folder_name]
        speed = geomean_results.get(folder_name, None)
        if folder_name == baseline_name:
            speed = 1.0
        speed_str = f"{speed:.4f}" if speed is not None else 'n/a'
        print(f"{folder_name:<30} {par2:<12.6f} {solved:>8}/{total:<8} {speed_str:<12}")
    
    # Generate PAR-2 comparison chart PDF
    plot_comparison_charts(folder_metrics, shared_par2_scores, shared_tests, 
                          exclusion_table, args.timeout, args.output_dir)
    # Generate cactus plot in separate PDF
    plot_cactus_chart(folder_metrics, shared_tests, args.timeout, args.output_dir)
    # Generate geomean speedup chart in separate PDF
    plot_geomean_chart(folder_metrics, shared_tests, args.timeout, args.output_dir, baseline_name, geomean_results)


if __name__ == "__main__":
    main()
