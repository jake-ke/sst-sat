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


RAW_COMPONENTS = ['heap', 'varactivity', 'clauses', 'variables', 'watches']

CATEGORY_COMPONENTS = {
    'watchlist': ['watches'],
    'clauses': ['clauses'],
    'priority_queue': ['heap', 'varactivity'],
    'variables': ['variables'],
}

CATEGORY_LABELS = {
    'watchlist': 'Watchlist',
    'clauses': 'Clauses',
    'priority_queue': 'P-Queue',
    'variables': 'Variables',
}


def format_count(n):
    """Format a large integer count with SI suffix."""
    n = float(n)
    for unit, div in (('T', 1e12), ('G', 1e9), ('M', 1e6), ('K', 1e3)):
        if abs(n) >= div:
            return f"{n/div:.2f}{unit}"
    return f"{n:.0f}"


def compute_cache_access_stats(results):
    """Compute per-test-averaged access counts and miss rates.

    For each finished test, derives raw per-component access/miss counts,
    per-component miss rates, and per-component contribution shares of that
    test's total. The per-folder summary is the unweighted mean across tests,
    so every test contributes equally (consistent with compute_cache_miss_rates).

    Returns dict with keys:
        mean_accesses, mean_misses, overall_miss_rate,
        categories: {cat -> {accesses, misses, miss_rate,
                             access_share, miss_share}}
    where accesses/misses are per-test means and rates/shares are unweighted
    averages of per-test percentages.
    """
    finished = [r for r in results if r.get('result') not in ('ERROR', 'UNKNOWN')]
    if not finished:
        return None

    per_test_total_acc = []
    per_test_total_mis = []
    per_test_overall_mr = []
    per_test_cat = {
        cat: {'accesses': [], 'misses': [], 'miss_rate': [],
              'access_share': [], 'miss_share': []}
        for cat in CATEGORY_COMPONENTS
    }

    for r in finished:
        raw_total = {c: (r.get(f'l1_{c}_total', 0) or 0) for c in RAW_COMPONENTS}
        raw_miss = {c: (r.get(f'l1_{c}_misses', 0) or 0) for c in RAW_COMPONENTS}
        test_total = sum(raw_total.values())
        test_miss = sum(raw_miss.values())
        if test_total == 0:
            continue

        per_test_total_acc.append(test_total)
        per_test_total_mis.append(test_miss)
        per_test_overall_mr.append((test_miss / test_total) * 100.0)

        for cat, comps in CATEGORY_COMPONENTS.items():
            cat_tot = sum(raw_total[c] for c in comps)
            cat_mis = sum(raw_miss[c] for c in comps)
            per_test_cat[cat]['accesses'].append(cat_tot)
            per_test_cat[cat]['misses'].append(cat_mis)
            per_test_cat[cat]['miss_rate'].append(
                (cat_mis / cat_tot) * 100.0 if cat_tot > 0 else 0.0)
            per_test_cat[cat]['access_share'].append(
                (cat_tot / test_total) * 100.0)
            per_test_cat[cat]['miss_share'].append(
                (cat_mis / test_miss) * 100.0 if test_miss > 0 else 0.0)

    if not per_test_total_acc:
        return None

    def mean(lst):
        return sum(lst) / len(lst) if lst else 0.0

    categories = {}
    for cat in CATEGORY_COMPONENTS:
        categories[cat] = {
            'accesses': mean(per_test_cat[cat]['accesses']),
            'misses': mean(per_test_cat[cat]['misses']),
            'miss_rate': mean(per_test_cat[cat]['miss_rate']),
            'access_share': mean(per_test_cat[cat]['access_share']),
            'miss_share': mean(per_test_cat[cat]['miss_share']),
        }

    return {
        'mean_accesses': mean(per_test_total_acc),
        'mean_misses': mean(per_test_total_mis),
        'overall_miss_rate': mean(per_test_overall_mr),
        'categories': categories,
        'n_tests': len(per_test_total_acc),
    }


def print_access_comparison(folder_data, folder_names):
    """Print per-folder per-test-averaged access/miss counts plus deltas."""
    print("\n" + "=" * 100)
    print("Per-Data-Structure Access Count & Miss Rate Comparison "
          "(per-test mean, each test weighted equally)")
    print("=" * 100)

    for folder_name in folder_names:
        stats = folder_data[folder_name].get('access_stats')
        if stats is None:
            print(f"\n[{folder_name}] No access stats available.")
            continue
        print(f"\n[{folder_name}]  tests={stats['n_tests']}")
        print(f"  Mean accesses/test: {format_count(stats['mean_accesses']):>10}  "
              f"({stats['mean_accesses']:,.0f})")
        print(f"  Mean misses/test  : {format_count(stats['mean_misses']):>10}  "
              f"({stats['mean_misses']:,.0f})")
        print(f"  Overall miss rate : {stats['overall_miss_rate']:.3f}%")
        header = (f"    {'Category':<10} {'Accesses':>10} {'Acc %':>8} "
                  f"{'Misses':>10} {'Miss %':>8} {'Miss Rate':>11}")
        print(header)
        print("    " + "-" * (len(header) - 4))
        for cat in ['watchlist', 'clauses', 'priority_queue', 'variables']:
            c = stats['categories'][cat]
            print(f"    {CATEGORY_LABELS[cat]:<10} "
                  f"{format_count(c['accesses']):>10} "
                  f"{c['access_share']:>7.2f}% "
                  f"{format_count(c['misses']):>10} "
                  f"{c['miss_share']:>7.2f}% "
                  f"{c['miss_rate']:>10.3f}%")

    # Side-by-side delta when exactly two folders
    if len(folder_names) == 2:
        a, b = folder_names
        sa = folder_data[a].get('access_stats')
        sb = folder_data[b].get('access_stats')
        if sa and sb:
            print("\n" + "-" * 100)
            print(f"Delta: {b} vs {a}  (accesses: relative %, miss rate: absolute pp)")
            print("-" * 100)
            rows = [('Overall',
                     sa['mean_accesses'], sb['mean_accesses'],
                     sa['overall_miss_rate'], sb['overall_miss_rate'])]
            for cat in ['watchlist', 'clauses', 'priority_queue', 'variables']:
                ca = sa['categories'][cat]
                cb = sb['categories'][cat]
                rows.append((CATEGORY_LABELS[cat],
                             ca['accesses'], cb['accesses'],
                             ca['miss_rate'], cb['miss_rate']))
            print(f"  {'Category':<10} {'Acc A':>10} {'Acc B':>10} {'Acc Δ':>10} "
                  f"{'MR A':>9} {'MR B':>9} {'MR Δ (pp)':>11}")
            print("  " + "-" * 72)
            for label, acc_a, acc_b, mr_a, mr_b in rows:
                acc_delta = ((acc_b - acc_a) / acc_a * 100.0) if acc_a > 0 else 0.0
                mr_delta = mr_b - mr_a
                print(f"  {label:<10} "
                      f"{format_count(acc_a):>10} "
                      f"{format_count(acc_b):>10} "
                      f"{acc_delta:>+9.2f}% "
                      f"{mr_a:>8.3f}% "
                      f"{mr_b:>8.3f}% "
                      f"{mr_delta:>+10.3f}")


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
        access_stats = compute_cache_access_stats(common_results)

        if miss_rates is None:
            print(f"  Error: Could not compute miss rates for {folder_name}")
            sys.exit(1)

        folder_data[folder_name] = {
            'miss_rates': miss_rates,
            'access_stats': access_stats,
            'test_count': len(common_results)
        }
    
    # Print summary table
    print(f"\n{'Folder':<30} {'Overall':<12} {'Watchlist':<12} {'Clauses':<12} {'Priority Queue':<15} {'Variables':<12}")
    print("-" * 100)
    for folder_name in folder_names:
        miss_rates = folder_data[folder_name]['miss_rates']
        print(f"{folder_name:<30} {miss_rates['overall']:>10.2f}% {miss_rates['watchlist']:>10.2f}% "
              f"{miss_rates['clauses']:>10.2f}% {miss_rates['priority_queue']:>13.2f}% {miss_rates['variables']:>10.2f}%")

    # Print per-data-structure access counts and exact miss rates
    print_access_comparison(folder_data, folder_names)

    # Generate plot
    plot_cache_comparison(folder_data, folder_names, args.output_dir)


if __name__ == "__main__":
    main()
