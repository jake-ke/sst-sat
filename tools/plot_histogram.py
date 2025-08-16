#!/usr/bin/env python3
"""
Plot Parallel Watchers and Variables Histograms from SAT solver logs.

This script:
1. Parses log files containing histogram data using unified_parser
2. Generates bar charts showing watchers and variables parallelism distribution
3. Exports detailed CSV with histogram data and problem characteristics
"""

import os
import sys
import csv
import argparse
import numpy as np
import matplotlib.pyplot as plt
from pathlib import Path
from collections import defaultdict
from unified_parser import parse_log_directory


def create_histogram_plots(data_points, output_dir='.'):
    """
    Create bar plots for watchers and variables histograms.
    
    Args:
        data_points: List of parsed log data dictionaries
        output_dir: Directory to save plots
    """
    print("Generating histogram plots...")
    
    # Filter logs with histogram data
    watchers_data = [d for d in data_points if 'watchers_bins' in d]
    variables_data = [d for d in data_points if 'variables_bins' in d]
    occupancy_data = [d for d in data_points if 'watchers_occupancy_bins' in d]
    
    if not watchers_data:
        print("No watchers histogram data available")
        return
        
    if not variables_data:
        print("No variables histogram data available")
        return
    
    # Create figure with two or three subplots depending on occupancy availability
    if occupancy_data:
        fig, axes = plt.subplots(3, 1, figsize=(14, 16))
        ax1, ax2, ax3 = axes
    else:
        fig, axes = plt.subplots(2, 1, figsize=(14, 12))
        ax1, ax2 = axes
    
    # Average the percentages across all logs
    # For watchers histogram
    watchers_bins = {}
    watchers_counts = {}
    max_watcher_bin = 0
    
    for log in watchers_data:
        bins = log.get('watchers_bins', {})
        for bin_key, values in bins.items():
            if isinstance(bin_key, str) and '-' in bin_key:
                start, end = map(int, bin_key.split('-'))
                max_watcher_bin = max(max_watcher_bin, end)
            else:
                max_watcher_bin = max(max_watcher_bin, int(bin_key) if bin_key != 'out_of_bounds' else 0)
                
            if bin_key not in watchers_bins:
                watchers_bins[bin_key] = []
                watchers_counts[bin_key] = []
            watchers_bins[bin_key].append(values['percentage'])
            watchers_counts[bin_key].append(values['samples'])
    
    # For variables histogram
    variables_bins = {}
    variables_counts = {}
    max_variable_bin = 0
    
    for log in variables_data:
        bins = log.get('variables_bins', {})
        for bin_key, values in bins.items():
            if isinstance(bin_key, str) and '-' in bin_key:
                start, end = map(int, bin_key.split('-'))
                max_variable_bin = max(max_variable_bin, end)
            else:
                max_variable_bin = max(max_variable_bin, int(bin_key) if bin_key != 'out_of_bounds' else 0)
                
            if bin_key not in variables_bins:
                variables_bins[bin_key] = []
                variables_counts[bin_key] = []
            variables_bins[bin_key].append(values['percentage'])
            variables_counts[bin_key].append(values['samples'])
    
    # Calculate averages for each bin
    watchers_avg = {bin_key: np.mean(percentages) for bin_key, percentages in watchers_bins.items()}
    variables_avg = {bin_key: np.mean(percentages) for bin_key, percentages in variables_bins.items()}
    
    # Calculate average counts
    watchers_count_avg = {bin_key: np.mean(counts) for bin_key, counts in watchers_counts.items()}
    variables_count_avg = {bin_key: np.mean(counts) for bin_key, counts in variables_counts.items()}
    
    # Sort bins for ordered plotting
    sorted_watchers_bins = sorted(watchers_avg.items(), key=lambda x: (
        float('inf') if x[0] == 'out_of_bounds' else 
        (int(x[0].split('-')[0]) if isinstance(x[0], str) and '-' in x[0] else int(x[0]))
    ))
    
    sorted_variables_bins = sorted(variables_avg.items(), key=lambda x: (
        float('inf') if x[0] == 'out_of_bounds' else 
        (int(x[0].split('-')[0]) if isinstance(x[0], str) and '-' in x[0] else int(x[0]))
    ))
    
    # Prepare data for plotting
    watchers_x = []
    watchers_y = []
    watchers_labels = []
    watchers_counts = []
    
    for bin_key, avg_percent in sorted_watchers_bins:
        if bin_key == 'out_of_bounds':
            watchers_x.append(len(watchers_x))
            watchers_labels.append('Out of\nbounds')
        else:
            watchers_x.append(len(watchers_x))
            watchers_labels.append(str(bin_key))
        watchers_y.append(avg_percent)
        watchers_counts.append(watchers_count_avg[bin_key])
    
    variables_x = []
    variables_y = []
    variables_labels = []
    variables_counts = []
    
    for bin_key, avg_percent in sorted_variables_bins:
        if bin_key == 'out_of_bounds':
            variables_x.append(len(variables_x))
            variables_labels.append('Out of\nbounds')
        else:
            variables_x.append(len(variables_x))
            variables_labels.append(str(bin_key))
        variables_y.append(avg_percent)
        variables_counts.append(variables_count_avg[bin_key])
    
    # Plot Watchers Histogram - use consistent color for all bars
    bars1 = ax1.bar(watchers_x, watchers_y, alpha=0.8, color='steelblue', 
                   edgecolor='black', linewidth=0.5)
    ax1.set_xticks(watchers_x)
    ax1.set_xticklabels(watchers_labels, rotation=45)
    ax1.set_xlabel('Number of Watchers')
    ax1.set_ylabel('Percentage of Samples (%)')
    ax1.set_title(f'Average Parallel Watchers Distribution (from {len(watchers_data)} logs)')
    ax1.grid(True, axis='y', alpha=0.3)
    
    # Add both count and percentage labels on top of each bar
    for i, (bar, count) in enumerate(zip(bars1, watchers_counts)):
        height = bar.get_height()
        ax1.annotate(f'{height:.1f}%\n({count:.0f})',
                     xy=(bar.get_x() + bar.get_width() / 2, height),
                     xytext=(0, 3),  # 3 points vertical offset
                     textcoords="offset points",
                     ha='center', va='bottom',
                     fontsize=9, fontweight='bold')
    
    # Plot Variables Histogram - use consistent color for all bars
    bars2 = ax2.bar(variables_x, variables_y, alpha=0.8, color='indianred',
                   edgecolor='black', linewidth=0.5)
    ax2.set_xticks(variables_x)
    ax2.set_xticklabels(variables_labels, rotation=45)
    ax2.set_xlabel('Number of Variables')
    ax2.set_ylabel('Percentage of Samples (%)')
    ax2.set_title(f'Average Parallel Variables Distribution (from {len(variables_data)} logs)')
    ax2.grid(True, axis='y', alpha=0.3)
    
    # Add both count and percentage labels on top of each bar
    for i, (bar, count) in enumerate(zip(bars2, variables_counts)):
        height = bar.get_height()
        ax2.annotate(f'{height:.1f}%\n({count:.0f})',
                     xy=(bar.get_x() + bar.get_width() / 2, height),
                     xytext=(0, 3),  # 3 points vertical offset
                     textcoords="offset points",
                     ha='center', va='bottom',
                     fontsize=9, fontweight='bold')

    # Plot Watchers Occupancy Histogram if available
    if occupancy_data:
        occ_bins = {}
        occ_counts = {}
        for log in occupancy_data:
            bins = log.get('watchers_occupancy_bins', {})
            for bin_key, values in bins.items():
                if bin_key not in occ_bins:
                    occ_bins[bin_key] = []
                    occ_counts[bin_key] = []
                occ_bins[bin_key].append(values['percentage'])
                occ_counts[bin_key].append(values['samples'])

        # Compute averages
        occ_avg = {k: np.mean(v) for k, v in occ_bins.items()}
        occ_count_avg = {k: np.mean(v) for k, v in occ_counts.items()}

        # Sort bins for ordered plotting
        sorted_occ_bins = sorted(occ_avg.items(), key=lambda x: (
            float('inf') if x[0] == 'out_of_bounds' else 
            (int(x[0].split('-')[0]) if isinstance(x[0], str) and '-' in x[0] else int(x[0]))
        ))

        occ_x, occ_y, occ_labels, occ_counts_list = [], [], [], []
        for bin_key, avg_percent in sorted_occ_bins:
            if bin_key == 'out_of_bounds':
                occ_x.append(len(occ_x))
                occ_labels.append('Out of\nbounds')
            else:
                occ_x.append(len(occ_x))
                occ_labels.append(str(bin_key))
            occ_y.append(avg_percent)
            occ_counts_list.append(occ_count_avg[bin_key])

        bars3 = ax3.bar(occ_x, occ_y, alpha=0.8, color='darkseagreen',
                        edgecolor='black', linewidth=0.5)
        ax3.set_xticks(occ_x)
        ax3.set_xticklabels(occ_labels, rotation=45)
        ax3.set_xlabel('Watchers per Clause (occupancy)')
        ax3.set_ylabel('Percentage of Samples (%)')
        ax3.set_title(f'Average Watchers Occupancy Distribution (from {len(occupancy_data)} logs)')
        ax3.grid(True, axis='y', alpha=0.3)

        for i, (bar, count) in enumerate(zip(bars3, occ_counts_list)):
            height = bar.get_height()
            ax3.annotate(f'{height:.1f}%\n({count:.0f})',
                         xy=(bar.get_x() + bar.get_width() / 2, height),
                         xytext=(0, 3),
                         textcoords="offset points",
                         ha='center', va='bottom',
                         fontsize=9, fontweight='bold')
    
    plt.tight_layout()
    plt.savefig(os.path.join(output_dir, 'parallel_histograms.png'), dpi=300, bbox_inches='tight')
    print(f"Histogram plots saved to: {os.path.join(output_dir, 'parallel_histograms.png')}")
    plt.close()


def export_histogram_csv(data_points, output_file='histogram_data.csv'):
    """
    Export histogram data to CSV.
    
    Args:
        data_points: List of parsed log data dictionaries
        output_file: Path to output CSV file
    """
    print(f"Exporting histogram data to {output_file}...")
    
    # Filter logs with histogram data
    filtered_data = [d for d in data_points if 'watchers_bins' in d or 'variables_bins' in d]
    
    if not filtered_data:
        print("No histogram data available to export")
        return
    
    # Find the max bin number across all logs for both watchers and variables
    max_watcher_bin = 0
    max_variable_bin = 0
    
    for log in filtered_data:
        if 'watchers_bins' in log:
            for bin_key in log['watchers_bins']:
                if bin_key == 'out_of_bounds':
                    continue
                if isinstance(bin_key, str) and '-' in bin_key:
                    start, end = map(int, bin_key.split('-'))
                    max_watcher_bin = max(max_watcher_bin, end)
                else:
                    max_watcher_bin = max(max_watcher_bin, int(bin_key))
        
        if 'variables_bins' in log:
            for bin_key in log['variables_bins']:
                if bin_key == 'out_of_bounds':
                    continue
                if isinstance(bin_key, str) and '-' in bin_key:
                    start, end = map(int, bin_key.split('-'))
                    max_variable_bin = max(max_variable_bin, end)
                else:
                    max_variable_bin = max(max_variable_bin, int(bin_key))
    
    # Prepare headers for the CSV file
    basic_headers = [
        'test_case', 'result', 'variables', 'clauses', 
        'total_memory_bytes', 'total_memory_formatted',
        'watchers_total_samples', 'variables_total_samples'
    ]
    
    # Add headers for watchers data (counts and percentages)
    watchers_count_headers = [f'watchers_{i}_count' for i in range(1, max_watcher_bin + 1)]
    watchers_count_headers.append('watchers_out_of_bounds_count')
    watchers_pct_headers = [f'watchers_{i}_pct' for i in range(1, max_watcher_bin + 1)]
    watchers_pct_headers.append('watchers_out_of_bounds_pct')
    
    # Add headers for variables data (counts and percentages)
    variables_count_headers = [f'variables_{i}_count' for i in range(1, max_variable_bin + 1)]
    variables_count_headers.append('variables_out_of_bounds_count')
    variables_pct_headers = [f'variables_{i}_pct' for i in range(1, max_variable_bin + 1)]
    variables_pct_headers.append('variables_out_of_bounds_pct')
    
    # Combine all headers
    all_headers = (
        basic_headers + 
        watchers_count_headers + watchers_pct_headers +
        variables_count_headers + variables_pct_headers
    )
    
    # Prepare data rows
    rows = []
    
    for log in filtered_data:
        row = {header: '' for header in all_headers}  # Initialize empty row
        
        # Fill basic info
        for field in basic_headers:
            if field in log:
                row[field] = log[field]
        
        # Fill watchers data
        if 'watchers_bins' in log:
            for bin_key, values in log['watchers_bins'].items():
                if bin_key == 'out_of_bounds':
                    row['watchers_out_of_bounds_count'] = values['samples']
                    row['watchers_out_of_bounds_pct'] = values['percentage']
                else:
                    # Handle bin ranges and single bins
                    if isinstance(bin_key, str) and '-' in bin_key:
                        start, end = map(int, bin_key.split('-'))
                        for i in range(start, end + 1):
                            # For ranges, distribute samples equally across the range
                            count_per_bin = values['samples'] / (end - start + 1)
                            pct_per_bin = values['percentage'] / (end - start + 1)
                            row[f'watchers_{i}_count'] = count_per_bin
                            row[f'watchers_{i}_pct'] = pct_per_bin
                    else:
                        bin_num = int(bin_key)
                        row[f'watchers_{bin_num}_count'] = values['samples']
                        row[f'watchers_{bin_num}_pct'] = values['percentage']
        
        # Fill variables data
        if 'variables_bins' in log:
            for bin_key, values in log['variables_bins'].items():
                if bin_key == 'out_of_bounds':
                    row['variables_out_of_bounds_count'] = values['samples']
                    row['variables_out_of_bounds_pct'] = values['percentage']
                else:
                    # Handle bin ranges and single bins
                    if isinstance(bin_key, str) and '-' in bin_key:
                        start, end = map(int, bin_key.split('-'))
                        for i in range(start, end + 1):
                            # For ranges, distribute samples equally across the range
                            count_per_bin = values['samples'] / (end - start + 1)
                            pct_per_bin = values['percentage'] / (end - start + 1)
                            row[f'variables_{i}_count'] = count_per_bin
                            row[f'variables_{i}_pct'] = pct_per_bin
                    else:
                        bin_num = int(bin_key)
                        row[f'variables_{bin_num}_count'] = values['samples']
                        row[f'variables_{bin_num}_pct'] = values['percentage']
        
        rows.append(row)
    
    # Write CSV file
    with open(output_file, 'w', newline='') as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=all_headers)
        writer.writeheader()
        writer.writerows(rows)
    
    print(f"Exported {len(rows)} records to {output_file}")


def main():
    """Main function to process log files and create plots/CSV."""
    parser = argparse.ArgumentParser(
        description="Plot parallel watchers and variables histograms from SAT solver log files"
    )
    parser.add_argument("log_dir", help="Directory containing log files to analyze")
    parser.add_argument("--csv", dest="csv_file", default="histogram_data.csv",
                       help="CSV file to export histogram data")
    parser.add_argument("--plot", dest="plot_file", default="parallel_histograms.png",
                       help="Output filename for histogram plots")
    parser.add_argument("--output-dir", dest="output_dir", default=".",
                       help="Directory to save outputs")
    
    args = parser.parse_args()
    
    # Create output directory if it doesn't exist
    os.makedirs(args.output_dir, exist_ok=True)
    
    # Process log files using unified parser
    data_points = parse_log_directory(args.log_dir, exclude_summary=True)
    
    if not data_points:
        print("No valid data found in the log files.")
        return 1
    
    print(f"Successfully processed {len(data_points)} log files")
    
    # Generate histogram plots
    create_histogram_plots(data_points, args.output_dir)
    
    # Export CSV data
    export_histogram_csv(data_points, os.path.join(args.output_dir, args.csv_file))
    
    print("Analysis complete!")


if __name__ == "__main__":
    main()
    