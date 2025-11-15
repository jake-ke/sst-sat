#!/usr/bin/env python3
"""
Plot Propagation Watcher and Literal Histograms from SAT solver logs.

This script:
1. Parses log files containing watcher and literal histogram data using unified_parser
2. Generates bar charts showing distribution of watchers and literals per propagation
3. Supports multi-seed directories
4. Averages over all finished tests (SAT/UNSAT)
5. Uses 11 bins with last bin labeled as "> {max_value}"
6. Labels percentage on top of each bar
"""

import sys
import os
import numpy as np
import matplotlib.pyplot as plt
from pathlib import Path
from unified_parser import parse_log_directory


def collect_histogram_data(log_dir):
    """
    Collect histogram data from log directory, supporting multi-seed layout.
    
    Args:
        log_dir: Path to directory containing log files or seed* subdirectories
    
    Returns:
        List of parsed log data dictionaries from finished tests
    """
    log_dir = Path(log_dir)
    
    if not log_dir.exists():
        print(f"Error: Directory {log_dir} does not exist")
        return []
    
    # Check for multi-seed layout
    seed_dirs = sorted([d for d in log_dir.glob('seed*') if d.is_dir()])
    
    all_results = []
    
    if seed_dirs:
        # Multi-seed mode
        print(f"Detected {len(seed_dirs)} seed folders")
        for seed_dir in seed_dirs:
            results = parse_log_directory(seed_dir, exclude_summary=True)
            if results:
                all_results.extend(results)
        print(f"Collected {len(all_results)} total tests from all seeds")
    else:
        # Single directory mode
        all_results = parse_log_directory(log_dir, exclude_summary=True)
        print(f"Collected {len(all_results)} tests")
    
    # Include finished and UNKNOWN tests (SAT/UNSAT/UNKNOWN)
    finished = [r for r in all_results if r.get('result') in ('SAT', 'UNSAT', 'UNKNOWN')]
    print(f"Included tests (SAT/UNSAT/UNKNOWN): {len(finished)}")
    
    return finished


def aggregate_histogram(results, histogram_key, num_bins=11):
    """Aggregate histogram data and compute per-bin index-weighted percentages.

    New semantics (per user request):
      - Exclude bin 0 entirely from display, but include it in the per-test denominator.
      - Define fixed display bins: 1..(threshold-1) and one merged bin '≥ threshold'.
      - For each test:
          * Compute per-bin percentage over total counts for that test (including bin 0).
          * Multiply each bin's percentage by its index; for the merged last bin sum
            contributions from detailed sub-bins with their own indices:
              - numeric bins ≥ threshold use their exact index
              - ranged bins (e.g., 10-19) use the midpoint (start+end)/2
              - out_of_bounds uses 25 (per user guidance)
        Then average these index-weighted percentages across tests.

    Returns:
        (labels, avg_original_percentages, avg_index_weighted_percentages, agg_raw_counts)
    """
    threshold = num_bins - 1  # e.g., for 11 bins, threshold=10, display bins are 1..9 and '≥ 10'
    inrange_indices = list(range(1, threshold))
    labels = [str(i) for i in inrange_indices] + [f'≥ {threshold}']

    # Accumulators for averaging (percentages)
    sum_orig = [0.0 for _ in labels]
    sum_index_weighted = [0.0 for _ in labels]
    num_tests = 0

    # Aggregated raw counts across all tests (for printing counts)
    agg_counts = {i: 0 for i in inrange_indices}
    agg_counts_oob = 0

    for r in results:
        bins = r.get(histogram_key, {}) or {}
        # Per-test counts
        counts = {i: 0 for i in inrange_indices}
        # Track detailed out-of-bound contributions for proper weighting
        # Each entry: (weight_index, count)
        oob_details = []
        oob_total_count = 0  # total samples in ≥ threshold including out_of_bounds
        bin0 = 0         # count for bin 0 (or 0-range)

        for bin_key, values in bins.items():
            count = values.get('samples', 0) or 0
            if count == 0:
                continue

            if bin_key == 'out_of_bounds':
                # Assign a representative index weight for OOB bucket
                # Per user guidance, use 25 for out_of_bounds
                oob_details.append((25.0, count))
                oob_total_count += count
            elif isinstance(bin_key, str) and '-' in bin_key:
                start_str, end_str = bin_key.split('-')
                try:
                    start = int(start_str)
                    end = int(end_str)
                except ValueError:
                    continue
                if start < 1:
                    if start == 0:
                        bin0 += count
                    continue  # drop negatives
                if start >= threshold:
                    # Use midpoint of the range as representative index
                    mid = (start + end) / 2.0
                    oob_details.append((mid, count))
                    oob_total_count += count
                else:
                    counts[start] = counts.get(start, 0) + count
            else:
                try:
                    idx = int(bin_key)
                except ValueError:
                    continue
                if idx < 1:
                    if idx == 0:
                        bin0 += count
                    continue
                if idx >= threshold:
                    oob_details.append((float(idx), count))
                    oob_total_count += count
                else:
                    counts[idx] = counts.get(idx, 0) + count

        # Aggregate raw counts
        for i in inrange_indices:
            agg_counts[i] += counts.get(i, 0)
        agg_counts_oob += oob_total_count

        # Totals (denominator): include bin0 + in-range + all OOB counts
        total = bin0 + sum(counts.values()) + oob_total_count
        if total == 0:
            continue

        # Per-test percentages (unweighted)
        per_orig = [ (counts[i] / total * 100.0) for i in inrange_indices ]
        per_orig_oob = (oob_total_count) / total * 100.0
        per_orig.append(per_orig_oob)

        # Index-weighted percentages (multiply each percentage by bin index)
        per_weighted = [ (i * (counts[i] / total * 100.0)) for i in inrange_indices ]
        # For ≥ threshold display bin, sum detailed contributions using their own indices/weights
        per_weighted_oob = sum((w * (cnt / total * 100.0)) for (w, cnt) in oob_details)
        per_weighted.append(per_weighted_oob)

        # Accumulate
        sum_orig = [a + b for a, b in zip(sum_orig, per_orig)]
        sum_index_weighted = [a + b for a, b in zip(sum_index_weighted, per_weighted)]
        num_tests += 1

    if num_tests == 0:
        return [], [], [], []

    avg_orig = [v / num_tests for v in sum_orig]
    avg_weighted = [v / num_tests for v in sum_index_weighted]

    # Build aggregated counts aligned to labels
    agg_counts_list = [agg_counts[i] for i in inrange_indices] + [agg_counts_oob]

    return labels, avg_orig, avg_weighted, agg_counts_list


def plot_propagation_histograms(results, output_pdf):
    """
    Create PDF plot with watcher and literal histograms in 2 subplots (top/bottom).
    
    Args:
        results: List of parsed log data dictionaries (finished tests only)
        output_pdf: Path to output PDF file
    """
    if not results:
        print("Error: No finished test data to plot")
        return
    
    # Create figure with 2 subplots (top and bottom)
    # Increase global font size for all text elements
    plt.rcParams.update({'font.size': 20})
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 10))
    
    # Plot 1: Watchers Histogram (top) - index-weighted percentages
    watcher_labels, watcher_original, watcher_weighted, watcher_counts = aggregate_histogram(results, 'watchers_bins', num_bins=11)
    
    if watcher_labels:
        x_pos = np.arange(len(watcher_labels))
        bars1 = ax1.bar(x_pos, watcher_weighted, alpha=0.8, color='steelblue', 
                         edgecolor='black', linewidth=1.0)
        ax1.set_xticks(x_pos)
        ax1.set_xticklabels(watcher_labels, fontsize=24)
        ax1.set_xlabel('# Watchers Processed per Literal', fontsize=26, fontweight='bold')
        ax1.set_ylabel('Weighted %', fontsize=26, fontweight='bold')
        ax1.tick_params(axis='y', which='major', labelsize=24)
        ax1.grid(True, axis='y', alpha=0.3, linestyle='--')
        
        # Add percentage labels on top of bars
        for bar, pct in zip(bars1, watcher_weighted):
            height = bar.get_height()
            ax1.text(bar.get_x() + bar.get_width() / 2, height,
                     f'{pct:.1f}%',
                     ha='center', va='bottom', fontsize=20, fontweight='bold')
        
        # Set y-axis limit with some headroom for labels
        max_pct = max(watcher_weighted) if watcher_weighted else 100
        ax1.set_ylim(0, max_pct * 1.20)
    else:
        ax1.text(0.5, 0.5, 'No watcher histogram data available',
                ha='center', va='center', transform=ax1.transAxes, fontsize=12)
        ax1.set_xlim(0, 1)
        ax1.set_ylim(0, 1)
    
    # Plot 2: Variables (Literals) Histogram (bottom) - index-weighted percentages
    variable_labels, variable_original, variable_weighted, variable_counts = aggregate_histogram(results, 'variables_bins', num_bins=11)
    
    if variable_labels:
        x_pos = np.arange(len(variable_labels))
        bars2 = ax2.bar(x_pos, variable_weighted, alpha=0.8, color='seagreen',
                         edgecolor='black', linewidth=1.0)
        ax2.set_xticks(x_pos)
        ax2.set_xticklabels(variable_labels, fontsize=24)
        ax2.set_xlabel('# Literals Residing in Trail', fontsize=26, fontweight='bold')
        ax2.set_ylabel('Weighted %', fontsize=26, fontweight='bold')
        ax2.tick_params(axis='y', which='major', labelsize=24)
        ax2.grid(True, axis='y', alpha=0.3, linestyle='--')
        
        # Add percentage labels on top of bars
        for bar, pct in zip(bars2, variable_weighted):
            height = bar.get_height()
            ax2.text(bar.get_x() + bar.get_width() / 2, height,
                     f'{pct:.1f}% ',
                     ha='center', va='bottom', fontsize=20, fontweight='bold')
        
        # Set y-axis limit with some headroom for labels
        max_pct = max(variable_weighted) if variable_weighted else 100
        ax2.set_ylim(0, max_pct * 1.20)
    else:
        ax2.text(0.5, 0.5, 'No variable histogram data available',
                ha='center', va='center', transform=ax2.transAxes, fontsize=12)
        ax2.set_xlim(0, 1)
        ax2.set_ylim(0, 1)
    
    plt.tight_layout()
    plt.savefig(output_pdf, format='pdf', bbox_inches='tight', dpi=300)
    print(f"\nPlot saved to: {output_pdf}")
    
    # Print summary statistics side by side with counts
    if watcher_labels:
        print("\nWatcher Distribution (counts aggregated across tests; % averaged across tests; denominator includes bin 0)")
        total_raw = sum(watcher_counts)
        print("  Bin    RawCount    Raw%   |   Weighted% (index × %) ")
        print("  ----   --------  -------  |   ----------------------")
        for label, rc, ro, w in zip(watcher_labels, watcher_counts, watcher_original, watcher_weighted):
            print(f"  {label:>6s}: {rc:8d}  {ro:7.2f}%  |   {w:7.2f}%")
        print(f"  {'Total':>6s}: {total_raw:8d}  {sum(watcher_original):7.2f}%  |   {sum(watcher_weighted):7.2f}%")
    
    if variable_labels:
        print("\nLiteral Distribution (counts aggregated across tests; % averaged across tests; denominator includes bin 0)")
        total_raw = sum(variable_counts)
        print("  Bin    RawCount    Raw%   |   Weighted% (index × %) ")
        print("  ----   --------  -------  |   ----------------------")
        for label, rc, ro, w in zip(variable_labels, variable_counts, variable_original, variable_weighted):
            print(f"  {label:>6s}: {rc:8d}  {ro:7.2f}%  |   {w:7.2f}%")
        print(f"  {'Total':>6s}: {total_raw:8d}  {sum(variable_original):7.2f}%  |   {sum(variable_weighted):7.2f}%")
    
    plt.close()


def main():
    if len(sys.argv) < 2:
        print("Usage: python plot_prop_histogram.py <logs_folder> [output.pdf]")
        print("Example: python plot_prop_histogram.py ../runs/logs prop_histogram.pdf")
        print("\nThe logs_folder can contain:")
        print("  - Log files directly")
        print("  - seed* subdirectories (multi-seed mode)")
        sys.exit(1)
    
    logs_folder = sys.argv[1]
    output_pdf = sys.argv[2] if len(sys.argv) > 2 else "prop_histogram.pdf"
    
    # Collect data from logs (supports multi-seed)
    finished_results = collect_histogram_data(logs_folder)
    
    if not finished_results:
        print("Error: No finished test data found")
        sys.exit(1)
    
    # Generate plots
    plot_propagation_histograms(finished_results, output_pdf)


if __name__ == "__main__":
    main()
