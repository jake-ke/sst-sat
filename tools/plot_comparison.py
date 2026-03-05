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

# Unified figure size for all charts - wider to accommodate compact legend
FIG_SIZE = (12, 6)
from unified_parser import parse_log_directory, format_bytes


# Manual exclusion list: add test case names here to exclude them from all comparisons
MANUAL_EXCLUSIONS = set([
    "080896c437245ac25eb6d3ad6df12c4f-bv-term-small-rw_1492.smt2.cnf",
    "e17d3f94f2c0e11ce6143bc4bf298bd7-mp1-qpr-bmp280-driver-5.cnf",
    "e185ebbd92c23ab460a3d29046eccf1d-group_mulr.cnf",
])

# Manual exclusive test set: if non-empty, ONLY these tests are considered for
# comparison logic (subject still to MANUAL_EXCLUSIONS removal). Any test listed
# here but missing/ERROR/UNKNOWN in a folder will appear in the exclusion table.
# NOTE: Use a list to preserve order for per-test speedup charts!
MANUAL_EXCLUSIVE_TESTS = [
    # >120k variables or >640k clauses
    # "06e928088bd822602edb83e41ce8dadb-satcoin-genesis-SAT-10.cnf",
    # "43e492bccfd57029b758897b17d7f04f-pb_300_09_lb_07.cnf",
    # "aacfb8797097f698d14337d3a04f3065-barman-pfile06-022.sas.ex.7.cnf",
    # "ff6b6ad55c0dffe034bc8daa9129ff1d-satcoin-genesis-SAT-10-sc2018.cnf",

    # > 1m clauses
    # "e185ebbd92c23ab460a3d29046eccf1d-group_mulr.cnf",  # did not finish
    # "7b5895f110a6aa5a5ac5d5a6eb3fd322-g2-ak128modasbg1sbisc.cnf",
    # "7aa3b29dde431cdacf17b2fb9a10afe4-Mario-t-hard-2_c18.cnf",
    # "fce130da1efd25fa9b12e59620a4146b-g2-ak128diagobg1btaig.cnf",
    # "91c69a1dedfa5f2b3779e38c48356593-Problem14_label20_true-unreach-call.c.cnf",
    # "1427381a809c64c721838894ece6756d-shuffling-2-s25242449-of-bench-sat04-727.used-as.sat04-753.cnf",
]

def get_folder_colors(folder_names):
    """Assign colors to folders with custom logic:
    - First folder (baseline): Red
    - "SATBlast" folder: Blue
    - Other folders: Colors in spectrum order (orange, yellow, green, cyan, purple, pink, etc.)
    
    Returns list of color hex codes in same order as folder_names.
    """
    # Color spectrum in order for smooth transitions
    spectrum_colors = [
        '#ff7f0e',  # Orange
        '#bcbd22',  # Yellow-green
        '#2ca02c',  # Green
        '#17becf',  # Cyan
        '#9467bd',  # Purple
        '#e377c2',  # Pink
        '#8c564b',  # Brown
        '#7f7f7f',  # Gray
    ]
    
    colors = []
    spectrum_idx = 0
    
    for idx, name in enumerate(folder_names):
        if idx == 0:
            # First folder is always red (baseline)
            colors.append('#d62728')
        elif 'SATBlast' in name or 'satblast' in name.lower():
            # SATBlast folder is always blue
            colors.append('#1f77b4')
        else:
            # Other folders use spectrum colors in order
            colors.append(spectrum_colors[spectrum_idx % len(spectrum_colors)])
            spectrum_idx += 1
    
    return colors


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


def get_shared_test_set(folder_metrics, timeout_seconds=None, exclude_timeouts=False, errors_as_timeout=False):
    """Determine the shared set of tests that finished (not ERROR/UNKNOWN) in all folders.
    
    Args:
        folder_metrics: dict of folder results
        timeout_seconds: timeout value in seconds (required if exclude_timeouts=True)
        exclude_timeouts: if True, also exclude TIMEOUT tests from shared set
        errors_as_timeout: if True, treat ERROR/UNKNOWN as timeout instead of excluding them
    
    Returns:
        tuple of (shared_tests: list, exclusion_table: list of tuples)
        shared_tests is an ordered list preserving MANUAL_EXCLUSIVE_TESTS order if specified
        exclusion_table contains (test_case, folder_name, reason) for excluded tests
    """
    if not folder_metrics:
        return [], []
    
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
    # Keep exclusive tests as list to preserve order!
    normalized_exclusive_list = [normalize_test_case(n) for n in MANUAL_EXCLUSIVE_TESTS]

    if normalized_exclusive_list:
        # Use the normalized exclusive list as candidate tests (preserve order, include even if missing to record exclusion)
        candidate_tests = normalized_exclusive_list
    else:
        candidate_tests = sorted(all_tests)
    
    # Determine shared tests (finished in ALL folders, optionally excluding timeouts)
    # Use list to preserve order from candidate_tests (which follows MANUAL_EXCLUSIVE_TESTS order if specified)
    shared_tests = []
    shared_tests_set = set()  # For fast lookup
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
            
            # Exclude ERROR/UNKNOWN unless errors_as_timeout is set
            if status in ('ERROR', 'UNKNOWN') and not errors_as_timeout:
                is_valid_in_all = False
                excluding_folder = folder_name
                excluding_reason = status
                break
            
            # Optionally exclude TIMEOUT
            if exclude_timeouts and timeout_ms is not None:
                # When errors_as_timeout is set, also check for ERROR/UNKNOWN
                is_timeout_like = (status == 'TIMEOUT' or 
                                  (status in ('SAT', 'UNSAT') and sim_ms > timeout_ms) or
                                  (errors_as_timeout and status in ('ERROR', 'UNKNOWN')))
                if is_timeout_like:
                    is_valid_in_all = False
                    excluding_folder = folder_name
                    excluding_reason = 'TIMEOUT'
                    break
        
        if is_valid_in_all:
            shared_tests.append(test_case)
            shared_tests_set.add(test_case)
        else:
            exclusion_table.append((test_case, excluding_folder, excluding_reason))
    
    return shared_tests, exclusion_table


def compute_par2_on_shared_set(folder_metrics, shared_tests, timeout_seconds, errors_as_timeout=False):
    """Compute PAR-2 scores using only the shared test set.
    
    Args:
        errors_as_timeout: if True, treat ERROR/UNKNOWN as timeout (1×timeout) instead of PAR-2
    
    Returns:
        dict mapping folder_name -> (par2_score, solved_count, total_count)
    """
    timeout_ms = timeout_seconds * 1000.0
    par2_penalty = 2 * timeout_ms
    timeout_penalty = timeout_ms  # 1× timeout for errors when errors_as_timeout=True
    
    par2_scores = {}
    
    for folder_name, metrics in folder_metrics.items():
        # Filter to shared tests only
        if errors_as_timeout:
            # Include ERROR/UNKNOWN when errors_as_timeout is set
            shared_results = [r for r in metrics['results'] 
                             if r.get('test_case') in shared_tests]
        else:
            # Exclude ERROR/UNKNOWN
            shared_results = [r for r in metrics['results'] 
                             if r.get('test_case') in shared_tests 
                             and r.get('result') not in ('ERROR', 'UNKNOWN')]
        
        if not shared_results:
            par2_scores[folder_name] = (None, 0, 0)
            continue
        
        par2_total = 0.0
        solved = 0
        
        for r in shared_results:
            result = r.get('result', 'UNKNOWN')
            
            if errors_as_timeout and result in ('ERROR', 'UNKNOWN'):
                # Treat ERROR/UNKNOWN as 1× timeout penalty
                par2_total += timeout_penalty
            elif result == 'TIMEOUT':
                par2_total += par2_penalty
            else:
                sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
                if result in ['SAT', 'UNSAT'] and sim_ms <= timeout_ms:
                    par2_total += sim_ms
                    solved += 1
                else:
                    par2_total += par2_penalty
        
        par2_score = (par2_total / len(shared_results)) / 1000.0
        par2_scores[folder_name] = (par2_score, solved, len(shared_results))
    
    return par2_scores


def compute_geomean_speedups(folder_metrics, shared_tests, timeout_seconds, baseline_name, errors_as_timeout=False):
    """Compute geometric mean speedup for each folder vs baseline using the shared test set.

    Speedup per test = t_baseline / t_config with times in milliseconds.
    - Uses PAR-2 effective times: timeouts or >timeout get 2*timeout penalty.
    - When errors_as_timeout=True, ERROR/UNKNOWN get 1×timeout penalty.
    - Computes over the provided shared_tests set (which may already have timeouts excluded).

    Returns: dict mapping folder_name -> geomean_speedup (float or None).
    Baseline has speedup 1.0.
    """
    timeout_ms = timeout_seconds * 1000.0
    penalty_ms = 2 * timeout_ms
    error_penalty_ms = timeout_ms if errors_as_timeout else penalty_ms

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
            # Apply appropriate penalty based on result type
            if b_res in ('SAT', 'UNSAT') and b_ms <= timeout_ms:
                tb_eff = b_ms
            elif errors_as_timeout and b_res in ('ERROR', 'UNKNOWN'):
                tb_eff = error_penalty_ms
            else:
                tb_eff = penalty_ms
            
            if c_res in ('SAT', 'UNSAT') and c_ms <= timeout_ms:
                tc_eff = c_ms
            elif errors_as_timeout and c_res in ('ERROR', 'UNKNOWN'):
                tc_eff = error_penalty_ms
            else:
                tc_eff = penalty_ms
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
                          exclusion_table, timeout_seconds, output_dir, exclude_timeouts=False, large_fonts=False):
    """Generate all comparison charts and save to PDF."""
    # Font scale factor: 1.5x larger when large_fonts is enabled
    font_scale = 1.2 if large_fonts else 1.0
    # Scale figure size proportionally to accommodate larger fonts
    fig_size = (FIG_SIZE[0] * (font_scale + 0.05), FIG_SIZE[1] * (font_scale + 0.05)) if large_fonts else FIG_SIZE
    
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    pdf_path = output_dir / 'comparison_charts.pdf'
    
    with matplotlib.backends.backend_pdf.PdfPages(pdf_path) as pdf:
        # Chart 1: PAR-2 Score Comparison (shared set)
        fig, ax = plt.subplots(figsize=fig_size)
        
        folder_names = list(folder_metrics.keys())
        par2_values = [shared_par2_scores[name][0] for name in folder_names]
        solved_counts = [shared_par2_scores[name][1] for name in folder_names]
        total_count = shared_par2_scores[folder_names[0]][2] if folder_names else 0
        # Grouped (clustered) bar chart with a single implicit category.
        # We suppress x-axis tick labels; legend conveys configuration names.
        num_folders = len(folder_names)
        # Adaptive bar width and spacing: narrower for 2-3 folders, wider for 4+
        if num_folders == 2:
            bar_width = 0.5
            bar_spacing = 0.7
        elif num_folders <= 4:
            bar_width = 0.7
            bar_spacing = 1.0
        else:
            bar_width = 1.4
            bar_spacing = 1.7  # Wider spacing to prevent overlap
        x_positions = [i * bar_spacing for i in range(num_folders)]
        folder_colors = get_folder_colors(folder_names)
        bars = ax.bar(x_positions, par2_values, width=bar_width, color=folder_colors, alpha=0.85, edgecolor='black', linewidth=0.8)
        # Y-axis label depends on whether timeouts are excluded
        y_label = 'Average Runtime (s)' if exclude_timeouts else 'PAR-2 (s)'
        ax.set_ylabel(y_label, fontsize=int(24 * font_scale))
        ax.set_xticks([])  # no x-axis categories shown
        ax.tick_params(axis='y', labelsize=int(28 * font_scale))
        ax.grid(axis='y', alpha=0.3)
        
        # Set y-axis limit to provide more space for labels and legend at top
        max_par2 = max(par2_values) if par2_values else 1
        # More space needed for legend when there are many folders (will wrap to multiple rows)
        y_limit_factor = 1.5 if num_folders > 5 else 1.35
        ax.set_ylim(0, max_par2 * y_limit_factor)  # Extra space for horizontal legend at top
        
        # Horizontal legend at top center
        ax.legend(bars, folder_names, loc='upper center', fontsize=int(24 * font_scale), frameon=False, 
                 ncol=min(num_folders, 5), bbox_to_anchor=(0.5, 1.02),
                 handlelength=1.0, handletextpad=0.5, columnspacing=1.0)
        
        # Add value labels on bars (PAR-2 and solved count side by side)
        for i, (bar, par2, solved) in enumerate(zip(bars, par2_values, solved_counts)):
            height = bar.get_height()
            ax.text(bar.get_x() + bar.get_width()/2., height,
                    f'{par2:.2f} s\nSolved: {solved}',
                    ha='center', va='bottom', fontsize=int(24 * font_scale))
        
        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)

        # Geomean speedup chart moved to separate PDF; no longer included here.
    
    print(f"\nPAR-2 chart saved to: {pdf_path}")


def plot_cactus_chart(folder_metrics, shared_tests, timeout_seconds, output_dir, pdf_basename='cactus_plot', large_fonts=False):
    """Generate a cactus (survival) plot: cumulative solved instances vs time for each configuration.
    Each line: sorted solve times (ms->s) of SAT/UNSAT results within timeout on the shared test set.
    Saved as a separate PDF.
    """
    # Font scale factor: 1.75x larger when large_fonts is enabled
    font_scale = 1.2 if large_fonts else 1.0
    # Scale figure size proportionally to accommodate larger fonts
    fig_size = (FIG_SIZE[0] * (font_scale + 0.05), FIG_SIZE[1] * (font_scale + 0.05)) if large_fonts else FIG_SIZE
    
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
        fig, ax = plt.subplots(figsize=fig_size)

        folder_colors = get_folder_colors(folder_names)
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
                    color=folder_colors[idx], label=folder_name)

        ax.set_xlabel('Wallclock Time (s)', fontsize=int(26 * font_scale))
        ax.set_ylabel('Solved Instances', fontsize=int(26 * font_scale))
        ax.tick_params(axis='both', labelsize=int(22 * font_scale))
        ax.grid(alpha=0.3, linestyle='--')
        # Compact legend without title, boxed
        ax.legend(loc='lower right', fontsize=int(26 * font_scale), frameon=True,
                 handlelength=1.5, handletextpad=0.5, borderaxespad=0.5, labelspacing=0.3)
        # No plot title per request.

        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)

    print(f"Cactus plot saved to: {pdf_path}")


def plot_geomean_chart(folder_metrics, shared_tests, timeout_seconds, output_dir, baseline_name, geomean_results, pdf_basename='geomean_speedups', large_fonts=False):
    """Plot geomean speedup bar chart in a separate PDF.
    geomean_results: dict folder_name -> geomean_speedup (float or None)
    """
    # Font scale factor: 1.75x larger when large_fonts is enabled
    font_scale = 1.2 if large_fonts else 1.0
    # Scale figure size proportionally to accommodate larger fonts
    fig_size = (FIG_SIZE[0] * (font_scale + 0.05), FIG_SIZE[1] * (font_scale + 0.05)) if large_fonts else FIG_SIZE
    
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
        fig, ax = plt.subplots(figsize=fig_size)
        num_folders = len(folder_names)
        folder_colors = get_folder_colors(folder_names)
        # Adaptive bar width and spacing: narrower for 2-3 folders, wider for 4+
        if num_folders == 2:
            bar_width = 0.5
            bar_spacing = 0.7
        elif num_folders <= 4:
            bar_width = 0.7
            bar_spacing = 1.0
        else:
            bar_width = 1.4
            bar_spacing = 1.7  # Wider spacing to prevent overlap
        x_positions = [i * bar_spacing for i in range(num_folders)]
        bars = ax.bar(x_positions, plot_vals, width=bar_width, color=folder_colors, alpha=0.85, edgecolor='black', linewidth=0.8)
        ax.set_ylabel('Geomean Speedup', fontsize=int(24 * font_scale))
        ax.set_xticks([])
        ax.tick_params(axis='y', labelsize=int(28 * font_scale))
        ax.grid(axis='y', alpha=0.3)
        
        # Set y-axis limit with space for horizontal legend at top
        ymax = max(plot_vals) if plot_vals else 1.0
        # More space needed for legend when there are many folders (will wrap to multiple rows)
        y_limit_factor = 1.5 if num_folders > 5 else 1.35
        ax.set_ylim(0, ymax * y_limit_factor)  # Extra space for legend
        
        # Always use horizontal legend at top center
        ax.legend(bars, folder_names, loc='upper center', fontsize=int(24 * font_scale), frameon=False, 
                 ncol=min(num_folders, 5), bbox_to_anchor=(0.5, 1.02),
                 handlelength=1.0, handletextpad=0.5, columnspacing=1.0)
        for bar, val in zip(bars, g_values):
            height = bar.get_height()
            label = 'n/a' if val is None else f'{val:.2f}×'
            ax.text(bar.get_x() + bar.get_width()/2., height,
                    label, ha='center', va='bottom', fontsize=int(26 * font_scale))
        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)
    print(f"Geomean speedup chart saved to: {pdf_path}")


def plot_per_test_speedups(folder_metrics, shared_tests, timeout_seconds, output_dir, baseline_name, geomean_results, pdf_basename='per_test_speedups', large_fonts=False):
    """Plot per-test speedup bars as grouped bars (one group per test + geomean group).
    Each group shows all folder configurations.
    Uses broken axis for high speedups (>50).
    Only generated when exclusive test set is specified.
    """
    # Font scale factor: 1.75x larger when large_fonts is enabled
    font_scale = 1.2 if large_fonts else 1.0
    # Scale figure size proportionally to accommodate larger fonts
    # Use half height for more compact per-test speedup chart
    per_test_fig_size = (FIG_SIZE[0] * font_scale, FIG_SIZE[1] / 2 * font_scale) if large_fonts else (FIG_SIZE[0], FIG_SIZE[1] / 2)
    
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    pdf_path = output_dir / f'{pdf_basename}.pdf'

    timeout_ms = timeout_seconds * 1000.0
    penalty_ms = 2 * timeout_ms

    folder_names = list(folder_metrics.keys())
    if baseline_name not in folder_names:
        print("Warning: baseline not in folder_metrics for per-test speedups")
        return

    # Build test_case -> folder_name -> effective_time mapping
    # shared_tests is already ordered (from MANUAL_EXCLUSIVE_TESTS if specified, otherwise sorted)
    ordered_tests = shared_tests  # shared_tests is now a list that preserves order
    
    test_times = {}
    for tc in ordered_tests:
        test_times[tc] = {}
        for folder_name, metrics in folder_metrics.items():
            for r in metrics['results']:
                if r.get('test_case') == tc:
                    try:
                        sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
                    except (TypeError, ValueError):
                        sim_ms = penalty_ms
                    result = r.get('result')
                    t_eff = sim_ms if (result in ('SAT', 'UNSAT') and sim_ms <= timeout_ms) else penalty_ms
                    test_times[tc][folder_name] = max(t_eff, 1e-6)
                    break

    # Compute per-test speedups for all configs vs baseline
    test_labels = [tc[:5] for tc in ordered_tests]  # First 5 chars
    test_labels.append('gmean')  # Add geomean group at the end
    
    num_folders = len(folder_names)
    num_groups = len(test_labels)  # tests + geomean
    
    # Build speedup matrix: [group][folder_idx] -> speedup
    speedup_matrix = []
    for tc in ordered_tests:
        row = []
        t_base = test_times[tc].get(baseline_name, penalty_ms)
        for folder_name in folder_names:
            t_cfg = test_times[tc].get(folder_name, penalty_ms)
            speedup = t_base / t_cfg if t_cfg > 0 else 0.0
            row.append(speedup)
        speedup_matrix.append(row)
    
    # Add geomean row at the end
    geomean_row = []
    for folder_name in folder_names:
        if folder_name == baseline_name:
            geomean_row.append(1.0)
        else:
            geomean_val = geomean_results.get(folder_name, None)
            geomean_row.append(geomean_val if geomean_val is not None else 0.0)
    speedup_matrix.append(geomean_row)

    # Check if we need broken axis (any value > 15)
    max_speedup = max(max(row) for row in speedup_matrix)
    use_broken_axis = max_speedup > 15

    with matplotlib.backends.backend_pdf.PdfPages(pdf_path) as pdf:
        if use_broken_axis:
            # Create figure with compact broken axis - just label high bars, don't show them
            fig, ax_bottom = plt.subplots(figsize=per_test_fig_size)
            ax_bottom.set_ylim(0, 15)
        else:
            fig, ax_bottom = plt.subplots(figsize=per_test_fig_size)
            ax_bottom.set_ylim(0, max_speedup * 1.2)
        
        bar_width = 0.9 / num_folders  # Wider bars (increased from 0.8 to 0.9)
        x_base = range(num_groups)
        
        # Get folder colors using the same scheme as other charts
        folder_colors = get_folder_colors(folder_names)
        
        bars_list = []
        for folder_idx, folder_name in enumerate(folder_names):
            x_positions = [x + folder_idx * bar_width for x in x_base]
            values = [speedup_matrix[group_idx][folder_idx] for group_idx in range(num_groups)]
            
            # Clip values at y-limit if using broken axis
            display_values = []
            for v in values:
                if use_broken_axis and v > 15:
                    display_values.append(15)
                else:
                    display_values.append(v)
            
            bars = ax_bottom.bar(x_positions, display_values, bar_width, 
                   label=folder_name, 
                   color=folder_colors[folder_idx],
                   alpha=0.9, edgecolor='black', linewidth=0.5)
            bars_list.append((bars, values, x_positions))
        
        # Label bars that exceed the limit - smaller text inside the bar
        if use_broken_axis:
            for folder_idx, (bars, values, x_positions) in enumerate(bars_list):
                for bar_idx, (bar, val, x_pos) in enumerate(zip(bars, values, x_positions)):
                    if val > 15:
                        ax_bottom.text(x_pos, 14.5, f'{val:.0f}', ha='center', va='top', 
                                       fontsize=int(10 * font_scale), rotation=0, color='black')
        
        # Major and minor horizontal grids - thicker lines
        ax_bottom.yaxis.set_major_locator(plt.MultipleLocator(5))
        ax_bottom.yaxis.set_minor_locator(plt.MultipleLocator(1))
        ax_bottom.grid(axis='y', which='major', alpha=0.6, linestyle='-', linewidth=1.2)
        ax_bottom.grid(axis='y', which='minor', alpha=0.4, linestyle='-', linewidth=0.8)
        ax_bottom.set_axisbelow(True)
        
        # Red dashed line at 1.0
        ax_bottom.axhline(y=1.0, color='darkred', linestyle='--', linewidth=1.5, alpha=0.8, zorder=3)
        
        ax_bottom.tick_params(axis='y', labelsize=int(18 * font_scale))
        
        # Set labels and horizontal legend at top with extra y-space
        ax_bottom.set_ylabel('Speedup', fontsize=int(18 * font_scale))
        ax_bottom.set_xticks([x + bar_width * (num_folders - 1) / 2 for x in x_base])
        ax_bottom.set_xticklabels(test_labels, fontsize=int(18 * font_scale), ha='center')
        
        # Horizontal legend at top - more compact vertically with more space
        # More space needed for legend when there are many folders (will wrap to multiple rows)
        if use_broken_axis:
            y_limit = 23 if num_folders > 5 else 20  # More space for legend with many folders
            ax_bottom.set_ylim(0, y_limit)
        else:
            y_limit_factor = 1.45 if num_folders > 5 else 1.3
            ax_bottom.set_ylim(0, max_speedup * y_limit_factor)  # More space for legend
        ax_bottom.legend(loc='upper center', fontsize=int(18 * font_scale), frameon=False, ncol=min(num_folders, 5), 
                        bbox_to_anchor=(0.5, 1.02))
        
        # No title per request
        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)
    
    print(f"Per-test speedup chart saved to: {pdf_path}")


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
    parser.add_argument('--errors-as-timeout', action='store_true',
                       help='Treat ERROR/UNKNOWN as timeout (1×timeout penalty) instead of excluding them')
    parser.add_argument('--large-fonts', action='store_true',
                       help='Use much larger font sizes for all plots (useful when scaling down for side-by-side comparisons)')
    
    args = parser.parse_args()
    
    if len(args.folders) < 2:
        print("Error: Need at least 2 folders to compare")
        sys.exit(1)
    
    if args.names and len(args.names) != len(args.folders):
        print(f"Error: Number of names ({len(args.names)}) must match number of folders ({len(args.folders)})")
        sys.exit(1)
    
    print(f"Comparing {len(args.folders)} folders with timeout={args.timeout}s")
    print(f"Folders (in order): {', '.join(args.folders)}")
    
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
        
        print(f"\nProcessing {folder_name} ({folder_path})...")
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
    error_mode = "as timeout" if args.errors_as_timeout else "excluded"
    print(f"\nDetermining shared test set (exclude_timeouts={args.exclude_timeouts_geomean}, errors={error_mode})...")
    shared_tests, exclusion_table = get_shared_test_set(folder_metrics, args.timeout, args.exclude_timeouts_geomean, args.errors_as_timeout)
        
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
    shared_par2_scores = compute_par2_on_shared_set(folder_metrics, shared_tests, args.timeout, args.errors_as_timeout)
    
    # Geomean speedups (baseline = first folder)
    baseline_name = next(iter(folder_metrics.keys()))
    geomean_results = compute_geomean_speedups(folder_metrics, shared_tests, args.timeout, baseline_name, args.errors_as_timeout)
    
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
                          exclusion_table, args.timeout, args.output_dir, args.exclude_timeouts_geomean, args.large_fonts)
    # Generate cactus plot in separate PDF
    plot_cactus_chart(folder_metrics, shared_tests, args.timeout, args.output_dir, large_fonts=args.large_fonts)
    # Generate geomean speedup chart in separate PDF
    plot_geomean_chart(folder_metrics, shared_tests, args.timeout, args.output_dir, baseline_name, geomean_results, large_fonts=args.large_fonts)
    
    # Generate per-test speedup chart if exclusive set is specified
    if MANUAL_EXCLUSIVE_TESTS:
        print(f"\nGenerating per-test speedup charts (exclusive set specified with {len(MANUAL_EXCLUSIVE_TESTS)} tests)...")
        plot_per_test_speedups(folder_metrics, shared_tests, args.timeout, args.output_dir, baseline_name, geomean_results, large_fonts=args.large_fonts)


if __name__ == "__main__":
    main()
