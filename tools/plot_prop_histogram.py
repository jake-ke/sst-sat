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
    
    # Filter to finished tests only
    finished = [r for r in all_results if r.get('result') in ('SAT', 'UNSAT')]
    print(f"Finished tests (SAT/UNSAT): {len(finished)}")
    
    return finished


def aggregate_histogram(results, histogram_key, num_bins=11):
    """
    Aggregate histogram data across multiple test results.
    
    Args:
        results: List of parsed log dictionaries
        histogram_key: Key for the histogram ('watchers_bins' or 'variables_bins')
        num_bins: Number of bins to use (default: 11)
    
    Returns:
        Tuple of (bin_labels, percentages) where percentages add up to ~100%
        The last bin (bin 10) includes all samples from bins >= 10
    """
    # Aggregate sample counts across all results
    total_samples = 0
    bin_samples = {}  # Maps bin_key to total sample count
    
    for r in results:
        bins = r.get(histogram_key, {})
        if not bins:
            continue
        
        for bin_key, values in bins.items():
            samples = values.get('samples', 0)
            total_samples += samples
            
            if bin_key not in bin_samples:
                bin_samples[bin_key] = 0
            bin_samples[bin_key] += samples
    
    if total_samples == 0:
        return [], []
    
    # Process bins and aggregate bins >= (num_bins - 1) into the last bin
    aggregated_bins = {}
    last_bin_threshold = num_bins - 1  # For 11 bins, this is 10
    
    for bin_key, samples in bin_samples.items():
        if bin_key == 'out_of_bounds':
            # Add to last bin
            if last_bin_threshold not in aggregated_bins:
                aggregated_bins[last_bin_threshold] = 0
            aggregated_bins[last_bin_threshold] += samples
        elif isinstance(bin_key, str) and '-' in bin_key:
            # Range bin like "3-7"
            start, end = map(int, bin_key.split('-'))
            if start >= last_bin_threshold:
                # Entire range goes into last bin
                if last_bin_threshold not in aggregated_bins:
                    aggregated_bins[last_bin_threshold] = 0
                aggregated_bins[last_bin_threshold] += samples
            else:
                # Add to appropriate bin
                aggregated_bins[start] = aggregated_bins.get(start, 0) + samples
        else:
            # Single bin
            bin_num = int(bin_key)
            if bin_num >= last_bin_threshold:
                # Add to last bin
                if last_bin_threshold not in aggregated_bins:
                    aggregated_bins[last_bin_threshold] = 0
                aggregated_bins[last_bin_threshold] += samples
            else:
                aggregated_bins[bin_num] = aggregated_bins.get(bin_num, 0) + samples
    
    # Sort bins by numeric value
    sorted_bins = sorted(aggregated_bins.items())
    
    # Build labels and percentages
    bin_labels = []
    percentages = []
    
    for bin_num, samples in sorted_bins:
        if bin_num == last_bin_threshold:
            # Last bin includes all >= threshold
            bin_labels.append(f'≥ {last_bin_threshold}')
        else:
            bin_labels.append(str(bin_num))
        
        # Calculate percentage based on total samples
        percentage = (samples / total_samples) * 100.0
        percentages.append(percentage)
    
    return bin_labels, percentages


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
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 10))
    
    # Plot 1: Watchers Histogram (top)
    watcher_labels, watcher_percentages = aggregate_histogram(results, 'watchers_bins', num_bins=11)
    
    if watcher_labels:
        x_pos = np.arange(len(watcher_labels))
        bars1 = ax1.bar(x_pos, watcher_percentages, alpha=0.8, color='steelblue', 
                       edgecolor='black', linewidth=1.0)
        ax1.set_xticks(x_pos)
        ax1.set_xticklabels(watcher_labels, fontsize=18)
        ax1.set_xlabel('Number of Watchers per Propagation', fontsize=20, fontweight='bold')
        ax1.set_ylabel('Percentage (%)', fontsize=20, fontweight='bold')
        ax1.tick_params(axis='y', which='major', labelsize=18)
        ax1.grid(True, axis='y', alpha=0.3, linestyle='--')
        
        # Add percentage labels on top of bars
        for bar, pct in zip(bars1, watcher_percentages):
            height = bar.get_height()
            ax1.text(bar.get_x() + bar.get_width() / 2, height,
                    f'{pct:.1f}%',
                    ha='center', va='bottom', fontsize=16, fontweight='bold')
        
        # Set y-axis limit with some headroom for labels
        max_pct = max(watcher_percentages) if watcher_percentages else 100
        ax1.set_ylim(0, max_pct * 1.15)
    else:
        ax1.text(0.5, 0.5, 'No watcher histogram data available',
                ha='center', va='center', transform=ax1.transAxes, fontsize=12)
        ax1.set_xlim(0, 1)
        ax1.set_ylim(0, 1)
    
    # Plot 2: Variables (Literals) Histogram (bottom)
    variable_labels, variable_percentages = aggregate_histogram(results, 'variables_bins', num_bins=11)
    
    if variable_labels:
        x_pos = np.arange(len(variable_labels))
        bars2 = ax2.bar(x_pos, variable_percentages, alpha=0.8, color='seagreen',
                   edgecolor='black', linewidth=1.0)
        ax2.set_xticks(x_pos)
        ax2.set_xticklabels(variable_labels, fontsize=18)
        ax2.set_xlabel('Number of Literals per Propagation', fontsize=20, fontweight='bold')
        ax2.set_ylabel('Percentage (%)', fontsize=20, fontweight='bold')
        ax2.tick_params(axis='y', which='major', labelsize=18)
        ax2.grid(True, axis='y', alpha=0.3, linestyle='--')
        
        # Add percentage labels on top of bars
        for bar, pct in zip(bars2, variable_percentages):
            height = bar.get_height()
            ax2.text(bar.get_x() + bar.get_width() / 2, height,
                    f'{pct:.1f}%',
                    ha='center', va='bottom', fontsize=16, fontweight='bold')
        
        # Set y-axis limit with some headroom for labels
        max_pct = max(variable_percentages) if variable_percentages else 100
        ax2.set_ylim(0, max_pct * 1.15)
    else:
        ax2.text(0.5, 0.5, 'No variable histogram data available',
                ha='center', va='center', transform=ax2.transAxes, fontsize=12)
        ax2.set_xlim(0, 1)
        ax2.set_ylim(0, 1)
    
    plt.tight_layout()
    plt.savefig(output_pdf, format='pdf', bbox_inches='tight', dpi=300)
    print(f"\nPlot saved to: {output_pdf}")
    
    # Print summary statistics
    if watcher_labels:
        print(f"\nWatcher Distribution:")
        total_watcher_pct = sum(watcher_percentages)
        for label, pct in zip(watcher_labels, watcher_percentages):
            print(f"  {label:>6s}: {pct:6.2f}%")
        print(f"  {'Total':>6s}: {total_watcher_pct:6.2f}%")
    
    if variable_labels:
        print(f"\nLiteral Distribution:")
        total_variable_pct = sum(variable_percentages)
        for label, pct in zip(variable_labels, variable_percentages):
            print(f"  {label:>6s}: {pct:6.2f}%")
        print(f"  {'Total':>6s}: {total_variable_pct:6.2f}%")
    
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
