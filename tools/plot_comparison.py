#!/usr/bin/env python3
"""
SAT Solver Multi-Folder Comparison Plotter

This script compares results from multiple input folders and generates comparison charts.

Usage: python plot_comparison.py <folder1> <folder2> [folder3] [...] [--timeout SECONDS] [--output-dir DIR]

Examples:
    python plot_comparison.py runs/baseline runs/optimized --output-dir results/
    python plot_comparison.py logs_4MiB logs_8MiB logs_16MiB --timeout 36
"""

import sys
import csv
import argparse
from pathlib import Path
from collections import defaultdict
import matplotlib.pyplot as plt
import matplotlib.backends.backend_pdf
from unified_parser import parse_log_directory, format_bytes


# Manual exclusion list: add test case names here to exclude them from all comparisons
MANUAL_EXCLUSIONS = set([
    # '1427381a809c64c721838894ece6756d-shuffling-2-s25242449-of-bench-sat04-727.used-as.sat04-753.cnf',
    # '25a654a029421baed232de5e6e19c72e-mp1-qpr-bmp280-driver-14.cnf',
    # 'e17d3f94f2c0e11ce6143bc4bf298bd7-mp1-qpr-bmp280-driver-5.cnf',
    # 'e185ebbd92c23ab460a3d29046eccf1d-group_mulr.cnf',
    # Example: 'test_case_name_2',
])


def compute_metrics_for_folder(folder_path, timeout_seconds):
    """Parse a folder and compute key metrics.
    
    Returns:
        dict with keys:
        - 'results': list of parsed result dicts
        - 'par2_score': PAR-2 score in seconds (or None if no valid results)
        - 'solved_count': number solved within timeout
        - 'total_count': total valid problems (excludes ERROR/UNKNOWN)
        - 'excluded_tests': list of test cases with ERROR/UNKNOWN
    """
    folder_path = Path(folder_path)
    
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
            
            agg = {'test_case': case}
            
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


def get_shared_test_set(folder_metrics):
    """Determine the shared set of tests that finished (not ERROR/UNKNOWN) in all folders.
    
    Returns:
        tuple of (shared_tests: set, exclusion_table: list of tuples)
        exclusion_table contains (test_case, folder_name, reason) for excluded tests
    """
    if not folder_metrics:
        return set(), []
    
    # Build per-folder test status maps
    folder_test_status = {}
    for folder_name, metrics in folder_metrics.items():
        status_map = {}
        for r in metrics['results']:
            test_case = r.get('test_case', '')
            result = r.get('result', 'UNKNOWN')
            primary_label = result.split()[0] if result else 'UNKNOWN'
            status_map[test_case] = primary_label
        folder_test_status[folder_name] = status_map
    
    # Find all test cases present in any folder
    all_tests = set()
    for status_map in folder_test_status.values():
        all_tests.update(status_map.keys())
    
    # Determine shared tests (finished in ALL folders)
    shared_tests = set()
    exclusion_table = []
    
    for test_case in sorted(all_tests):
        # Check manual exclusion list first
        if test_case in MANUAL_EXCLUSIONS:
            exclusion_table.append((test_case, 'MANUAL', 'MANUAL_EXCLUSION'))
            continue
        
        is_finished_in_all = True
        excluding_folder = None
        excluding_reason = None
        
        for folder_name in sorted(folder_metrics.keys()):
            status_map = folder_test_status[folder_name]
            status = status_map.get(test_case, 'MISSING')
            
            if status in ('ERROR', 'UNKNOWN', 'MISSING'):
                is_finished_in_all = False
                excluding_folder = folder_name
                excluding_reason = status
                break
        
        if is_finished_in_all:
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


def plot_comparison_charts(folder_metrics, shared_par2_scores, shared_tests, 
                          exclusion_table, timeout_seconds, output_dir):
    """Generate all comparison charts and save to PDF."""
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    pdf_path = output_dir / 'comparison_charts.pdf'
    
    with matplotlib.backends.backend_pdf.PdfPages(pdf_path) as pdf:
        # Chart 1: PAR-2 Score Comparison (shared set)
        fig, ax = plt.subplots(figsize=(10, 6))
        
        folder_names = list(folder_metrics.keys())
        par2_values = [shared_par2_scores[name][0] for name in folder_names]
        solved_counts = [shared_par2_scores[name][1] for name in folder_names]
        total_count = shared_par2_scores[folder_names[0]][2] if folder_names else 0
        
        bars = ax.bar(range(len(folder_names)), par2_values, color='steelblue', alpha=0.7)
        ax.set_xlabel('Configuration', fontsize=12)
        ax.set_ylabel(f'PAR-2 Score (seconds, timeout={timeout_seconds}s)', fontsize=12)
        ax.set_title(f'PAR-2 Comparison on Shared Test Set ({total_count} tests)', fontsize=14, fontweight='bold')
        ax.set_xticks(range(len(folder_names)))
        ax.set_xticklabels(folder_names, rotation=45, ha='right')
        ax.grid(axis='y', alpha=0.3)
        
        # Add value labels on bars
        for i, (bar, par2, solved) in enumerate(zip(bars, par2_values, solved_counts)):
            height = bar.get_height()
            ax.text(bar.get_x() + bar.get_width()/2., height,
                   f'{par2:.1f}s\n({solved}/{total_count})',
                   ha='center', va='bottom', fontsize=9)
        
        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)
        
        # Chart 2: Solved Count Comparison
        fig, ax = plt.subplots(figsize=(10, 6))
        
        bars = ax.bar(range(len(folder_names)), solved_counts, color='forestgreen', alpha=0.7)
        ax.set_xlabel('Configuration', fontsize=12)
        ax.set_ylabel('Number of Tests Solved', fontsize=12)
        ax.set_title(f'Solved Tests Comparison (Shared Set: {total_count} tests)', fontsize=14, fontweight='bold')
        ax.set_xticks(range(len(folder_names)))
        ax.set_xticklabels(folder_names, rotation=45, ha='right')
        ax.set_ylim(0, total_count * 1.1)
        ax.grid(axis='y', alpha=0.3)
        
        # Add value labels
        for bar, solved in zip(bars, solved_counts):
            height = bar.get_height()
            ax.text(bar.get_x() + bar.get_width()/2., height,
                   f'{solved}\n({100*solved/total_count:.1f}%)',
                   ha='center', va='bottom', fontsize=9)
        
        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)
        
        # Chart 3: Exclusion Table (if there are exclusions)
        if exclusion_table:
            fig, ax = plt.subplots(figsize=(11, max(6, len(exclusion_table) * 0.15)))
            ax.axis('tight')
            ax.axis('off')
            
            # Create table data
            table_data = [['Test Case', 'Excluding Folder', 'Reason']]
            for test_case, folder, reason in exclusion_table:
                table_data.append([test_case, folder, reason])
            
            table = ax.table(cellText=table_data, cellLoc='left', loc='center',
                           colWidths=[0.5, 0.3, 0.2])
            table.auto_set_font_size(False)
            table.set_fontsize(8)
            table.scale(1, 1.5)
            
            # Style header row
            for i in range(3):
                table[(0, i)].set_facecolor('#4472C4')
                table[(0, i)].set_text_props(weight='bold', color='white')
            
            # Alternate row colors
            for i in range(1, len(table_data)):
                for j in range(3):
                    if i % 2 == 0:
                        table[(i, j)].set_facecolor('#F2F2F2')
            
            ax.set_title(f'Excluded Tests ({len(exclusion_table)} tests excluded from shared set)',
                        fontsize=14, fontweight='bold', pad=20)
            
            plt.tight_layout()
            pdf.savefig(fig, bbox_inches='tight')
            plt.close(fig)
        
        # Chart 4: Average Memory Usage
        fig, ax = plt.subplots(figsize=(10, 6))
        
        avg_memory = []
        for folder_name in folder_names:
            results = folder_metrics[folder_name]['results']
            shared_results = [r for r in results if r.get('test_case') in shared_tests]
            if shared_results:
                total_mem = sum(r.get('total_memory_bytes', 0) for r in shared_results)
                avg = total_mem / len(shared_results)
                avg_memory.append(avg)
            else:
                avg_memory.append(0)
        
        # Convert to MB for display
        avg_memory_mb = [m / (1024 * 1024) for m in avg_memory]
        
        bars = ax.bar(range(len(folder_names)), avg_memory_mb, color='coral', alpha=0.7)
        ax.set_xlabel('Configuration', fontsize=12)
        ax.set_ylabel('Average Memory Usage (MB)', fontsize=12)
        ax.set_title(f'Average Memory Usage Comparison (Shared Set: {total_count} tests)', fontsize=14, fontweight='bold')
        ax.set_xticks(range(len(folder_names)))
        ax.set_xticklabels(folder_names, rotation=45, ha='right')
        ax.grid(axis='y', alpha=0.3)
        
        # Add value labels
        for bar, mem_mb in zip(bars, avg_memory_mb):
            height = bar.get_height()
            ax.text(bar.get_x() + bar.get_width()/2., height,
                   f'{mem_mb:.1f} MB',
                   ha='center', va='bottom', fontsize=9)
        
        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)
        
        # Chart 5: Average Runtime (for solved tests only)
        fig, ax = plt.subplots(figsize=(10, 6))
        
        avg_runtime = []
        timeout_ms = timeout_seconds * 1000.0
        for folder_name in folder_names:
            results = folder_metrics[folder_name]['results']
            solved_results = [r for r in results 
                            if r.get('test_case') in shared_tests 
                            and r.get('result') in ('SAT', 'UNSAT')
                            and float(r.get('sim_time_ms', 0.0) or 0.0) <= timeout_ms]
            if solved_results:
                total_time = sum(float(r.get('sim_time_ms', 0.0) or 0.0) for r in solved_results)
                avg = total_time / len(solved_results)
                avg_runtime.append(avg)
            else:
                avg_runtime.append(0)
        
        bars = ax.bar(range(len(folder_names)), avg_runtime, color='mediumpurple', alpha=0.7)
        ax.set_xlabel('Configuration', fontsize=12)
        ax.set_ylabel('Average Runtime (ms)', fontsize=12)
        ax.set_title(f'Average Runtime for Solved Tests (Shared Set)', fontsize=14, fontweight='bold')
        ax.set_xticks(range(len(folder_names)))
        ax.set_xticklabels(folder_names, rotation=45, ha='right')
        ax.grid(axis='y', alpha=0.3)
        
        # Add value labels
        for bar, runtime in zip(bars, avg_runtime):
            height = bar.get_height()
            ax.text(bar.get_x() + bar.get_width()/2., height,
                   f'{runtime:.1f} ms',
                   ha='center', va='bottom', fontsize=9)
        
        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)
    
    print(f"\nAll charts saved to: {pdf_path}")


def main():
    parser = argparse.ArgumentParser(
        description='Compare SAT solver results from multiple folders',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s runs/baseline runs/optimized
  %(prog)s logs_4MiB logs_8MiB logs_16MiB --timeout 36 --output-dir results/
        """
    )
    
    parser.add_argument('folders', nargs='+', help='Input folders to compare (in display order)')
    parser.add_argument('--timeout', type=float, default=36, 
                       help='Timeout in seconds for PAR-2 calculation (default: 36)')
    parser.add_argument('--output-dir', default='results', 
                       help='Output directory for plots (default: results/)')
    
    args = parser.parse_args()
    
    if len(args.folders) < 2:
        print("Error: Need at least 2 folders to compare")
        sys.exit(1)
    
    print(f"Comparing {len(args.folders)} folders with timeout={args.timeout}s")
    print(f"Folders (in order): {', '.join(args.folders)}\n")
    
    # Compute metrics for each folder
    # Use OrderedDict to preserve order and handle duplicate names
    from collections import OrderedDict
    folder_metrics = OrderedDict()
    folder_name_counts = {}
    
    for folder_path in args.folders:
        base_name = Path(folder_path).name
        
        # Handle duplicate folder names by adding a counter
        if base_name in folder_name_counts:
            folder_name_counts[base_name] += 1
            folder_name = f"{base_name}_{folder_name_counts[base_name]}"
        else:
            folder_name_counts[base_name] = 0
            folder_name = base_name
        
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
    
    # Determine shared test set
    print(f"\nDetermining shared test set...")
    shared_tests, exclusion_table = get_shared_test_set(folder_metrics)
    print(f"Shared test set: {len(shared_tests)} tests")
    print(f"Excluded tests: {len(exclusion_table)}")
    
    if not shared_tests:
        print("Error: No shared tests found across all folders")
        sys.exit(1)
    
    # Compute PAR-2 on shared set
    print(f"\nComputing PAR-2 scores on shared set...")
    shared_par2_scores = compute_par2_on_shared_set(folder_metrics, shared_tests, args.timeout)
    
    print(f"\n{'Folder':<30} {'PAR-2 (s)':<12} {'Solved':<10} {'Total':<10}")
    print("-" * 62)
    for folder_name in folder_metrics.keys():
        par2, solved, total = shared_par2_scores[folder_name]
        print(f"{folder_name:<30} {par2:>11.2f} {solved:>9}/{total:<9}")
    
    if exclusion_table:
        print(f"\n=== Excluded Tests ({len(exclusion_table)} tests) ===")
        print(f"{'Test Case':<60} {'Excluding Folder':<30} {'Reason':<15}")
        print("-" * 105)
        for test_case, folder, reason in sorted(exclusion_table):
            print(f"{test_case:<60} {folder:<30} {reason:<15}")
    
    # Generate plots
    print(f"\nGenerating comparison charts...")
    plot_comparison_charts(folder_metrics, shared_par2_scores, shared_tests, 
                          exclusion_table, args.timeout, args.output_dir)
    
    print("\nComparison complete!")


if __name__ == "__main__":
    main()
