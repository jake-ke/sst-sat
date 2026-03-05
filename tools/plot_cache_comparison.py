#!/usr/bin/env python3
"""
Cache Miss Rate Comparison Plotter

This script compares L1 cache miss rates between two configurations using grouped bar charts.
Shows overall miss rate and breakdown by data structure.

Data structures compared:
- Overall: total cache miss rate
- Watchlist: watch list accesses
- Clauses: clause database accesses
- Priority Queue: combined heap + varactivity accesses
- Variables: variable array accesses

Usage: python plot_cache_comparison.py <folder1> <folder2> [--names "Name1" "Name2"] [--output-dir DIR]

Examples:
    python plot_cache_comparison.py runs/baseline runs/optimized
    python plot_cache_comparison.py logs_128KB logs_256KB --names "128KB" "256KB" --output-dir results/
"""

import sys
import argparse
from pathlib import Path
import matplotlib.pyplot as plt
import matplotlib.backends.backend_pdf
from unified_parser import parse_log_directory

# Figure size
FIG_SIZE = (9, 4)


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


def compute_cache_miss_rates(results):
    """Compute L1 cache miss rates across finished tests.
    
    Returns dict with:
    - overall: total miss rate (average total_requests and total_misses across tests)
    - watchlist: watchlist miss rate contribution
    - clauses: clauses miss rate contribution
    - priority_queue: priority queue (heap + varactivity) miss rate contribution
    - variables: variables miss rate contribution
    
    The overall miss rate is computed from the average of total_requests and total_misses
    across all finished tests. Each test's total is the sum of all data structures
    (heap + varactivity + clauses + variables + watches).
    
    The data structure contributions are computed as:
    (average_data_structure_misses / average_total_requests) * 100.0
    """
    # Filter to tests with valid data (exclude ERROR/UNKNOWN, but include TIMEOUT which has cache data)
    finished = [r for r in results if r.get('result') not in ('ERROR', 'UNKNOWN')]
    
    if not finished:
        return None
    

    # Compute per-test miss rates, then average them
    test_overall_miss_rates = []
    test_priority_queue_norm = []
    test_clauses_norm = []
    test_variables_norm = []
    test_watchlist_norm = []

    for r in finished:
        # Get component values for this test
        heap_total = r.get('l1_heap_total', 0) or 0
        heap_miss = r.get('l1_heap_misses', 0) or 0

        varactivity_total = r.get('l1_varactivity_total', 0) or 0
        varactivity_miss = r.get('l1_varactivity_misses', 0) or 0

        clauses_total = r.get('l1_clauses_total', 0) or 0
        clauses_miss = r.get('l1_clauses_misses', 0) or 0

        variables_total = r.get('l1_variables_total', 0) or 0
        variables_miss = r.get('l1_variables_misses', 0) or 0

        watches_total = r.get('l1_watches_total', 0) or 0
        watches_miss = r.get('l1_watches_misses', 0) or 0

        # Compute total for this test (for overall miss rate)
        test_total = heap_total + varactivity_total + clauses_total + variables_total + watches_total
        test_miss = heap_miss + varactivity_miss + clauses_miss + variables_miss + watches_miss

        # Overall miss rate
        if test_total > 0:
            test_overall_miss_rates.append((test_miss / test_total) * 100.0)

        # Priority queue: (heap + varactivity)
        pq_total = heap_total + varactivity_total
        pq_miss = heap_miss + varactivity_miss
        pq_norm = 0.0
        if pq_total > 0 and test_total > 0:
            pq_norm = (pq_miss / pq_total) * (pq_total / test_total) * 100.0
        test_priority_queue_norm.append(pq_norm)

        # Clauses
        clauses_norm = 0.0
        if clauses_total > 0 and test_total > 0:
            clauses_norm = (clauses_miss / clauses_total) * (clauses_total / test_total) * 100.0
        test_clauses_norm.append(clauses_norm)

        # Variables
        variables_norm = 0.0
        if variables_total > 0 and test_total > 0:
            variables_norm = (variables_miss / variables_total) * (variables_total / test_total) * 100.0
        test_variables_norm.append(variables_norm)

        # Watchlist
        watchlist_norm = 0.0
        if watches_total > 0 and test_total > 0:
            watchlist_norm = (watches_miss / watches_total) * (watches_total / test_total) * 100.0
        test_watchlist_norm.append(watchlist_norm)

    if not test_overall_miss_rates:
        return None

    # Average the per-test miss rates
    miss_rates = {
        'overall': sum(test_overall_miss_rates) / len(test_overall_miss_rates) if test_overall_miss_rates else 0.0,
        'priority_queue': sum(test_priority_queue_norm) / len(test_priority_queue_norm) if test_priority_queue_norm else 0.0,
        'clauses': sum(test_clauses_norm) / len(test_clauses_norm) if test_clauses_norm else 0.0,
        'variables': sum(test_variables_norm) / len(test_variables_norm) if test_variables_norm else 0.0,
        'watchlist': sum(test_watchlist_norm) / len(test_watchlist_norm) if test_watchlist_norm else 0.0,
    }

    return miss_rates


def parse_folder_cache_data(folder_path):
    """Parse a folder and compute cache miss rates.
    
    Returns:
        dict with keys:
        - 'miss_rates': dict of miss rates by category
        - 'test_count': number of finished tests
    """
    folder_path = Path(folder_path)
    
    # Check for multi-seed layout
    seed_dirs = [p for p in sorted(folder_path.glob('seed*')) if p.is_dir()]
    
    all_results = []
    if seed_dirs:
        # Multi-seed: aggregate all seeds
        print(f"  Found {len(seed_dirs)} seed directories")
        for sd in seed_dirs:
            results = parse_log_directory(sd, exclude_summary=True)
            if results:
                all_results.extend(results)
    else:
        # Single run
        all_results = parse_log_directory(folder_path, exclude_summary=True)
    
    if not all_results:
        return None
    
    finished = [r for r in all_results if r.get('result') not in ('ERROR', 'UNKNOWN')]
    print(f"  Total tests: {len(all_results)}, Finished: {len(finished)}")
    
    miss_rates = compute_cache_miss_rates(all_results)
    
    if miss_rates is None:
        return None
    
    return {
        'miss_rates': miss_rates,
        'test_count': len(finished)
    }


def plot_cache_comparison(folder_data, folder_names, output_dir):
    """Generate grouped bar chart comparing cache miss rates.
    
    Args:
        folder_data: dict mapping folder_name -> data dict
        folder_names: list of folder names in display order
        output_dir: output directory for PDF
    """
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    
    pdf_path = output_dir / 'cache_comparison.pdf'
    
    # Categories to plot (in order)
    categories = ['overall', 'watchlist', 'clauses', 'priority_queue', 'variables']
    category_labels = {
        'overall': 'Overall',
        'watchlist': 'Watchlist',
        'clauses': 'Clauses',
        'priority_queue': 'P-Queue',
        'variables': 'Variables'
    }
    
    with matplotlib.backends.backend_pdf.PdfPages(pdf_path) as pdf:
        fig, ax = plt.subplots(figsize=FIG_SIZE)
        
        num_folders = len(folder_names)
        num_categories = len(categories)
        bar_width = 0.8 / num_folders
        x_base = range(num_categories)
        
        # Use get_folder_colors for consistent color scheme
        folder_colors = get_folder_colors(folder_names)
        
        # Plot bars for each folder
        for folder_idx, folder_name in enumerate(folder_names):
            x_positions = [x + folder_idx * bar_width for x in x_base]
            miss_rates = folder_data[folder_name]['miss_rates']
            values = [miss_rates[cat] for cat in categories]
            
            ax.bar(x_positions, values, bar_width,
                   label=folder_name,
                   color=folder_colors[folder_idx],
                   alpha=0.85, edgecolor='black', linewidth=0.8)
        
        # Formatting
        ax.set_ylabel('Miss Rate (%)', fontsize=18)
        ax.set_xticks([x + bar_width * (num_folders - 1) / 2 for x in x_base])
        ax.set_xticklabels([category_labels[cat] for cat in categories], fontsize=18, ha='center')
        ax.tick_params(axis='y', labelsize=18)
        
        # Set up grid with major and minor ticks
        ax.grid(axis='y', alpha=0.6, linestyle='-', linewidth=1.2)
        ax.set_axisbelow(True)
        
        # Set y-axis limit with space for horizontal legend at top
        max_rate = max(max(folder_data[fn]['miss_rates'].values()) for fn in folder_names)
        y_limit_factor = 1.5 if num_folders > 5 else 1.35
        ax.set_ylim(0, max_rate * y_limit_factor)
        
        # Horizontal legend at top center
        ax.legend(loc='upper center', fontsize=18, frameon=False,
                 ncol=min(num_folders, 5), bbox_to_anchor=(0.5, 1.02),
                 handlelength=1.0, handletextpad=0.5, columnspacing=1.0)
        
        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)
    
    print(f"\nCache comparison chart saved to: {pdf_path}")


def main():
    parser = argparse.ArgumentParser(
        description='Compare L1 cache miss rates between two configurations',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s runs/baseline runs/optimized
  %(prog)s logs_128KB logs_256KB --names "128KB" "256KB" --output-dir results/
        """
    )
    
    parser.add_argument('folders', nargs=2, help='Two input folders to compare (in display order)')
    parser.add_argument('--names', nargs=2,
                       help='Custom names for each folder')
    parser.add_argument('--output-dir', default='results',
                       help='Output directory for plots (default: results/)')
    
    args = parser.parse_args()
    
    print(f"Comparing cache miss rates between 2 folders\n")
    
    folder_names = []
    all_results = {}
    
    # Parse all results once
    for i, folder_path in enumerate(args.folders):
        # Use custom name if provided, otherwise use folder name
        if args.names:
            folder_name = args.names[i]
        else:
            folder_name = Path(folder_path).name
        
        print(f"Processing {folder_name} ({folder_path})...")
        
        folder_path = Path(folder_path)
        seed_dirs = [p for p in sorted(folder_path.glob('seed*')) if p.is_dir()]
        
        if seed_dirs:
            all_folder_results = []
            print(f"  Found {len(seed_dirs)} seed directories")
            for sd in seed_dirs:
                results = parse_log_directory(sd, exclude_summary=True)
                if results:
                    all_folder_results.extend(results)
        else:
            all_folder_results = parse_log_directory(folder_path, exclude_summary=True)
        
        if not all_folder_results:
            print(f"  Error: No valid data found in {folder_path}")
            sys.exit(1)
        
        finished = [r for r in all_folder_results if r.get('result') not in ('ERROR', 'UNKNOWN')]
        print(f"  Total tests: {len(all_folder_results)}, Finished: {len(finished)}")
        
        folder_names.append(folder_name)
        all_results[folder_name] = {r['test_case']: r for r in all_folder_results}
    
    # Find tests that exist and are valid in all folders
    all_test_names = set()
    for results_map in all_results.values():
        all_test_names.update(results_map.keys())
    
    common_valid_tests = set()
    for test_name in all_test_names:
        is_valid_in_all = True
        for folder_name in folder_names:
            if test_name not in all_results[folder_name]:
                is_valid_in_all = False
                break
            result = all_results[folder_name][test_name].get('result')
            if result in ('ERROR', 'UNKNOWN'):
                is_valid_in_all = False
                break
        if is_valid_in_all:
            common_valid_tests.add(test_name)
    
    print(f"\nCommon valid tests across all folders: {len(common_valid_tests)}")
    
    # Recompute miss rates using only common valid tests
    folder_data = {}
    for folder_name in folder_names:
        common_results = [all_results[folder_name][tc] for tc in common_valid_tests]
        miss_rates = compute_cache_miss_rates(common_results)
        
        if miss_rates is None:
            print(f"  Error: Could not compute miss rates for {folder_name}")
            sys.exit(1)
        
        folder_data[folder_name] = {
            'miss_rates': miss_rates,
            'test_count': len(common_results)
        }
    
    # Print summary table
    print(f"\n{'Folder':<30} {'Overall':<12} {'Watchlist':<12} {'Clauses':<12} {'Priority Queue':<15} {'Variables':<12}")
    print("-" * 100)
    for folder_name in folder_names:
        miss_rates = folder_data[folder_name]['miss_rates']
        print(f"{folder_name:<30} {miss_rates['overall']:>10.2f}% {miss_rates['watchlist']:>10.2f}% "
              f"{miss_rates['clauses']:>10.2f}% {miss_rates['priority_queue']:>13.2f}% {miss_rates['variables']:>10.2f}%")
    
    # Generate plot
    plot_cache_comparison(folder_data, folder_names, args.output_dir)


if __name__ == "__main__":
    main()
