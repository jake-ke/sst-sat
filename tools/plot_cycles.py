#!/usr/bin/env python3
"""
Script to analyze and plot cycle statistics across different cache sizes.

This script:
1. Parses log files from different cache size directories using unified parser
2. Extracts cycle statistics from "Cycle Statistics" section
3. Calculates averages across all test cases for each cache size
4. Creates stacked line charts showing cycle breakdown by cache size
5. Exports detailed CSV with cycle data, L1 miss rates, and solver statistics
"""

import os
import numpy as np
import matplotlib.pyplot as plt
import csv
import argparse
from collections import defaultdict
from unified_parser import parse_log_directory, get_cache_size_from_directory


def collect_cycle_data_from_logs(base_dir):
    """
    Collect cycle data from all cache size directories.
    Returns dict: {cache_size: parsed_data_list}
    """
    cache_dirs = [
        'logs_4KiB', 'logs_8KiB', 'logs_16KiB', 'logs_32KiB', 'logs_64KiB', 'logs_128KiB',
        'logs_256KiB', 'logs_512KiB', 'logs_1MiB', 'logs_2MiB', 'logs_4MiB'
    ]
    
    data = {}
    
    for cache_dir in cache_dirs:
        cache_path = os.path.join(base_dir, cache_dir)
        if not os.path.exists(cache_path):
            print(f"Warning: Directory {cache_path} not found")
            continue
        
        cache_size = get_cache_size_from_directory(cache_dir)
        if cache_size is None:
            print(f"Warning: Could not parse cache size from {cache_dir}")
            continue
        
        print(f"Processing {cache_dir} (cache size: {cache_size} bytes)...")
        
        # Parse all log files using unified parser
        results = parse_log_directory(cache_path, exclude_summary=True)
        
        if not results:
            print(f"Warning: No valid log files found in {cache_path}")
            continue
        
        # Filter results that have cycle data
        cycle_results = [r for r in results if r.get('total_counted_cycles', 0) > 0]
        
        if not cycle_results:
            print(f"Warning: No cycle data found in {cache_path}")
            continue
        
        data[cache_size] = cycle_results
        print(f"  Processed {len(cycle_results)} files with cycle data")
    
    return data


def calculate_cycle_averages(data):
    """
    Calculate average cycle counts for each cache size.
    Returns:
    - avg_cycles: {cache_size: {cycle_type: avg_cycles}}
    - avg_total_cycles: {cache_size: avg_total_cycles}
    """
    avg_cycles = {}
    avg_total_cycles = {}
    
    # Cycle types to analyze
    cycle_types = [
        'propagate_cycles', 'analyze_cycles', 'minimize_cycles', 
        'backtrack_cycles', 'decision_cycles', 'reduce_db_cycles', 'restart_cycles'
    ]
    
    for cache_size, results in data.items():
        avg_cycles[cache_size] = {}
        
        # Calculate average total cycles
        total_cycles = [r.get('total_counted_cycles', 0) for r in results]
        avg_total_cycles[cache_size] = np.mean(total_cycles)
        
        # Calculate average for each cycle type
        for cycle_type in cycle_types:
            cycle_counts = [r.get(cycle_type, 0) for r in results]
            avg_cycles[cache_size][cycle_type] = np.mean(cycle_counts)
    
    return avg_cycles, avg_total_cycles


def create_cycle_plots(avg_cycles, avg_total_cycles, data, output_dir='.'):
    """Create stacked line charts showing cycle breakdown by cache size."""
    
    # Get sorted cache sizes
    cache_sizes = sorted(avg_cycles.keys())
    
    # Convert cache sizes to more readable format
    cache_size_labels = []
    for size in cache_sizes:
        if size >= 1024*1024:
            cache_size_labels.append(f"{size//(1024*1024)}MiB")
        elif size >= 1024:
            cache_size_labels.append(f"{size//1024}KiB")
        else:
            cache_size_labels.append(f"{size}B")
    
    # Cycle types for plotting
    cycle_types = [
        'propagate_cycles', 'analyze_cycles', 'minimize_cycles', 
        'backtrack_cycles', 'decision_cycles', 'reduce_db_cycles', 'restart_cycles'
    ]
    
    # Nice labels for display
    cycle_labels = [
        'Propagate', 'Analyze', 'Minimize', 
        'Backtrack', 'Decision', 'Reduce DB', 'Restart'
    ]
    
    # Colors for different cycle types
    colors = [
        '#1f77b4',  # Blue - Propagate
        '#ff7f0e',  # Orange - Analyze  
        '#2ca02c',  # Green - Minimize
        '#d62728',  # Red - Backtrack
        '#9467bd',  # Purple - Decision
        '#8c564b',  # Brown - Reduce DB
        '#e377c2'   # Pink - Restart
    ]
    
    # Create figure with three subplots
    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(12, 15))
    
    # Plot 1: Stacked area chart showing absolute cycle counts
    cycle_data = np.zeros((len(cycle_types), len(cache_sizes)))
    for i, cycle_type in enumerate(cycle_types):
        for j, cache_size in enumerate(cache_sizes):
            cycle_data[i, j] = avg_cycles[cache_size].get(cycle_type, 0)
    
    ax1.stackplot(cache_size_labels, *cycle_data, 
                  labels=cycle_labels, colors=colors, alpha=0.8)
    
    # Plot total cycles line on top
    total_cycles = [avg_total_cycles.get(cs, 0) for cs in cache_sizes]
    ax1.plot(cache_size_labels, total_cycles, marker='s', linewidth=3, 
            label='Total Cycles', color='black', markersize=8, linestyle='--')
    
    ax1.set_xlabel('Cache Size')
    ax1.set_ylabel('Average Cycles')
    ax1.set_title('Average Cycle Breakdown by Cache Size (Absolute)')
    ax1.legend(loc='upper left', bbox_to_anchor=(1.02, 1))
    ax1.grid(True, alpha=0.3)
    ax1.tick_params(axis='x', rotation=45)
    
    # Plot 2: Individual cycle stage percentages as line plots
    for i, (cycle_type, cycle_label) in enumerate(zip(cycle_types, cycle_labels)):
        percentages = []
        for cache_size in cache_sizes:
            total = avg_total_cycles.get(cache_size, 1)  # Avoid division by zero
            cycle_count = avg_cycles[cache_size].get(cycle_type, 0)
            percentage = (cycle_count / total) * 100 if total > 0 else 0
            percentages.append(percentage)
        
        ax2.plot(cache_size_labels, percentages, marker='o', linewidth=2, 
                label=cycle_label, color=colors[i])
    
    ax2.set_xlabel('Cache Size')
    ax2.set_ylabel('Percentage of Total Cycles (%)')
    ax2.set_title('Individual Cycle Stage Percentages by Cache Size')
    ax2.legend(loc='upper left', bbox_to_anchor=(1.02, 1))
    ax2.grid(True, alpha=0.3)
    ax2.tick_params(axis='x', rotation=45)
    ax2.set_ylim(0, max([max([
        (avg_cycles[cs].get(ct, 0) / avg_total_cycles.get(cs, 1)) * 100 
        for cs in cache_sizes
    ]) for ct in cycle_types]) + 5)  # Set y-limit based on max percentage + 5%
    
    # Plot 3: Geometric mean of total cycle counts across cache sizes
    geomean_cycles = []
    for cache_size in cache_sizes:
        # Get all cycle counts for this cache size from the data
        cache_results = []
        for cache_sz, results in data.items():
            if cache_sz == cache_size:
                cycle_counts = [r.get('total_counted_cycles', 0) for r in results if r.get('total_counted_cycles', 0) > 0]
                cache_results = cycle_counts
                break
        
        if cache_results:
            # Calculate geometric mean (using numpy's implementation)
            geomean = np.exp(np.mean(np.log(cache_results)))
            geomean_cycles.append(geomean)
        else:
            geomean_cycles.append(0)
    
    ax3.plot(cache_size_labels, geomean_cycles, marker='o', linewidth=3, 
            color='#2E86AB', markersize=8)
    
    ax3.set_xlabel('Cache Size')
    ax3.set_ylabel('Geometric Mean Total Cycles')
    ax3.set_title('Geometric Mean of Total Cycle Counts by Cache Size')
    ax3.grid(True, alpha=0.3)
    ax3.tick_params(axis='x', rotation=45)
    
    plt.tight_layout()
    
    # Save plots
    output_path = os.path.join(output_dir, 'cycle_breakdown_by_cache_size.png')
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    print(f"Cycle plots saved to: {output_path}")
    
    plt.show()


def get_dominant_cycle_stage(result):
    """Determine which cycle stage has the most cycles for a given result."""
    cycle_types = [
        ('propagate_cycles', 'Propagate'),
        ('analyze_cycles', 'Analyze'),
        ('minimize_cycles', 'Minimize'),
        ('backtrack_cycles', 'Backtrack'),
        ('decision_cycles', 'Decision'),
        ('reduce_db_cycles', 'Reduce_DB'),
        ('restart_cycles', 'Restart')
    ]
    
    max_cycles = 0
    dominant_stage = 'Unknown'
    
    for cycle_key, stage_name in cycle_types:
        cycles = result.get(cycle_key, 0)
        if cycles > max_cycles:
            max_cycles = cycles
            dominant_stage = stage_name
    
    return dominant_stage


def export_summary_csv(avg_cycles, avg_total_cycles, output_dir='.'):
    """Export summary CSV with averages by cache size."""
    
    csv_path = os.path.join(output_dir, 'cycle_analysis_summary.csv')
    
    # Get sorted cache sizes
    cache_sizes = sorted(avg_cycles.keys())
    
    # Cycle types
    cycle_types = [
        'propagate_cycles', 'analyze_cycles', 'minimize_cycles', 
        'backtrack_cycles', 'decision_cycles', 'reduce_db_cycles', 'restart_cycles'
    ]
    
    # Define fieldnames
    fieldnames = ['cache_size_label', 'cache_size_bytes', 'total_avg_cycles']
    fieldnames.extend([f'avg_{ct}' for ct in cycle_types])
    fieldnames.extend([f'pct_{ct.replace("_cycles", "")}' for ct in cycle_types])
    
    # Prepare data
    csv_data = []
    
    for cache_size in cache_sizes:
        # Convert cache size to readable format
        if cache_size >= 1024*1024:
            cache_size_label = f"{cache_size//(1024*1024)}MiB"
        elif cache_size >= 1024:
            cache_size_label = f"{cache_size//1024}KiB"
        else:
            cache_size_label = f"{cache_size}B"
        
        total_avg = avg_total_cycles.get(cache_size, 0)
        
        row = {
            'cache_size_label': cache_size_label,
            'cache_size_bytes': cache_size,
            'total_avg_cycles': total_avg
        }
        
        # Add average cycle counts
        for cycle_type in cycle_types:
            avg_cycles_val = avg_cycles[cache_size].get(cycle_type, 0)
            row[f'avg_{cycle_type}'] = avg_cycles_val
            
            # Add percentage
            if total_avg > 0:
                percentage = (avg_cycles_val / total_avg) * 100
            else:
                percentage = 0
            row[f'pct_{cycle_type.replace("_cycles", "")}'] = percentage
        
        csv_data.append(row)
    
    # Write CSV file
    with open(csv_path, 'w', newline='') as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(csv_data)
    
    print(f"Summary CSV exported to: {csv_path}")


def export_individual_cache_csvs(data, output_dir='.'):
    """Export each cache size's data to its own CSV file."""
    
    # Define fieldnames for individual cache CSV files - same order as detailed CSV
    fieldnames = [
        # Basic info
        'test_case', 'result', 'variables', 'clauses', 'total_counted_cycles', 'bottleneck',
        
        # Cycle stage percentages (2 decimal places)
        'propagate_percent', 'analyze_percent', 'minimize_percent', 'backtrack_percent',
        'decision_percent', 'reduce_db_percent', 'restart_percent',
        
        # L1 cache statistics
        'l1_total_requests', 'l1_total_miss_rate',
        'l1_heap_miss_rate', 'l1_variables_miss_rate', 'l1_watches_miss_rate',
        'l1_clauses_miss_rate', 'l1_varactivity_miss_rate',
        
        # Solver statistics (only requested ones)
        'decisions', 'propagations', 'conflicts', 'learned', 'removed',
        'db_reductions', 'minimized', 'restarts'
    ]
    
    for cache_size, results in data.items():
        # Convert cache size to readable format for filename
        if cache_size >= 1024*1024:
            cache_size_label = f"{cache_size//(1024*1024)}MiB"
        elif cache_size >= 1024:
            cache_size_label = f"{cache_size//1024}KiB"
        else:
            cache_size_label = f"{cache_size}B"
        
        # Create CSV filename
        csv_filename = f'cycle_analysis_{cache_size_label}.csv'
        csv_path = os.path.join(output_dir, csv_filename)
        
        # Collect data for this cache size
        csv_data = []
        
        for result in results:
            total_cycles = result.get('total_counted_cycles', 0)
            
            # Calculate percentages for each cycle stage (2 decimal places)
            cycle_percentages = {}
            cycle_types = ['propagate', 'analyze', 'minimize', 'backtrack', 'decision', 'reduce_db', 'restart']
            for cycle_type in cycle_types:
                cycles = result.get(f'{cycle_type}_cycles', 0)
                if total_cycles > 0:
                    percentage = round((cycles / total_cycles) * 100, 2)
                else:
                    percentage = 0.0
                cycle_percentages[f'{cycle_type}_percent'] = percentage
            
            row = {
                'test_case': result.get('test_case', ''),
                'result': result.get('result', ''),
                'variables': result.get('variables', 0),
                'clauses': result.get('clauses', 0),
                'total_counted_cycles': total_cycles,
                'bottleneck': get_dominant_cycle_stage(result),
                
                # Cycle stage percentages
                'propagate_percent': cycle_percentages['propagate_percent'],
                'analyze_percent': cycle_percentages['analyze_percent'],
                'minimize_percent': cycle_percentages['minimize_percent'],
                'backtrack_percent': cycle_percentages['backtrack_percent'],
                'decision_percent': cycle_percentages['decision_percent'],
                'reduce_db_percent': cycle_percentages['reduce_db_percent'],
                'restart_percent': cycle_percentages['restart_percent'],
                
                # L1 cache statistics
                'l1_total_requests': result.get('l1_total_requests', 0),
                'l1_total_miss_rate': result.get('l1_total_miss_rate', 0),
                'l1_heap_miss_rate': result.get('l1_heap_miss_rate', 0),
                'l1_variables_miss_rate': result.get('l1_variables_miss_rate', 0),
                'l1_watches_miss_rate': result.get('l1_watches_miss_rate', 0),
                'l1_clauses_miss_rate': result.get('l1_clauses_miss_rate', 0),
                'l1_varactivity_miss_rate': result.get('l1_varactivity_miss_rate', 0),
                
                # Solver statistics
                'decisions': result.get('decisions', 0),
                'propagations': result.get('propagations', 0),
                'conflicts': result.get('conflicts', 0),
                'learned': result.get('learned', 0),
                'removed': result.get('removed', 0),
                'db_reductions': result.get('db_reductions', 0),
                'minimized': result.get('minimized', 0),
                'restarts': result.get('restarts', 0)
            }
            
            csv_data.append(row)
        
        # Sort by bottleneck (dominant cycle stage), then by total cycle count descending
        csv_data.sort(key=lambda x: (x['bottleneck'], -x['total_counted_cycles']))
        
        # Write CSV file
        with open(csv_path, 'w', newline='') as csvfile:
            writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
            writer.writeheader()
            writer.writerows(csv_data)
        
        print(f"Cache size {cache_size_label} CSV exported to: {csv_path} ({len(csv_data)} records)")


def create_individual_test_cycle_plot(data, output_dir='.'):
    """Create a plot showing each individual test's total cycle count over cache size, grouped by total cycle count."""
    
    # Get sorted cache sizes
    cache_sizes = sorted(data.keys())
    
    # Convert cache sizes to more readable format
    cache_size_labels = []
    for size in cache_sizes:
        if size >= 1024*1024:
            cache_size_labels.append(f"{size//(1024*1024)}MiB")
        elif size >= 1024:
            cache_size_labels.append(f"{size//1024}KiB")
        else:
            cache_size_labels.append(f"{size}B")
    
    # Collect all test cycle counts for each cache size
    all_test_names = set()
    test_data = {}  # {test_name: {cache_size: total_cycles}}
    
    # First pass: collect all unique test names and their data
    for cache_size, results in data.items():
        cycle_results = [r for r in results if r.get('total_counted_cycles', 0) > 0]
        
        for result in cycle_results:
            test_name = result['test_case']
            all_test_names.add(test_name)
            
            if test_name not in test_data:
                test_data[test_name] = {}
            
            test_data[test_name][cache_size] = result['total_counted_cycles']
    
    # Calculate average total cycles for each test across all cache sizes
    test_avg_cycles = {}
    for test_name in all_test_names:
        cycle_counts = []
        for cache_size in cache_sizes:
            if cache_size in test_data[test_name]:
                cycle_counts.append(test_data[test_name][cache_size])
        
        if cycle_counts:
            test_avg_cycles[test_name] = np.mean(cycle_counts)
    
    # Sort tests by average total cycles and divide into 3 groups
    sorted_tests = sorted(test_avg_cycles.items(), key=lambda x: x[1])
    total_tests = len(sorted_tests)
    
    group_size = total_tests // 3
    group1 = [test[0] for test in sorted_tests[:group_size]]  # Low cycles
    group2 = [test[0] for test in sorted_tests[group_size:2*group_size]]  # Medium cycles
    group3 = [test[0] for test in sorted_tests[2*group_size:]]  # High cycles
    
    # Create figure with 3 subplots
    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(12, 12))
    
    # Define a diverse color palette for each subplot
    diverse_colors = [
        '#1f77b4', '#ff7f0e', '#2ca02c', '#d62728', '#9467bd', '#8c564b', '#e377c2', '#7f7f7f',
        '#bcbd22', '#17becf', '#aec7e8', '#ffbb78', '#98df8a', '#ff9896', '#c5b0d5', '#c49c94',
        '#f7b6d3', '#c7c7c7', '#dbdb8d', '#9edae5', '#393b79', '#637939', '#8c6d31', '#843c39',
        '#7b4173', '#5254a3', '#8ca252', '#bd9e39', '#ad494a', '#a55194'
    ]
    
    def plot_group(ax, group_tests, group_name):
        for i, test_name in enumerate(group_tests):
            cycle_counts = []
            valid_labels = []
            
            for j, cache_size in enumerate(cache_sizes):
                if cache_size in test_data[test_name]:
                    cycle_counts.append(test_data[test_name][cache_size])
                    valid_labels.append(cache_size_labels[j])
            
            if len(cycle_counts) > 1:  # Only plot if we have data for multiple cache sizes
                color = diverse_colors[i % len(diverse_colors)]
                ax.plot(valid_labels, cycle_counts, linewidth=2.5, alpha=0.8, marker='o', 
                       color=color, markersize=5)
        
        ax.set_xlabel('Cache Size')
        ax.set_ylabel('Total Cycles')
        ax.set_title(f'{group_name} ({len(group_tests)} tests)')
        ax.grid(True, alpha=0.3)
        ax.tick_params(axis='x', rotation=45)
    
    # Plot each group
    if group1:
        avg_cycles_group1 = np.mean([test_avg_cycles[test] for test in group1])
        plot_group(ax1, group1, f'Low Cycle Count (avg: {avg_cycles_group1:.0f})')
    
    if group2:
        avg_cycles_group2 = np.mean([test_avg_cycles[test] for test in group2])
        plot_group(ax2, group2, f'Medium Cycle Count (avg: {avg_cycles_group2:.0f})')
    
    if group3:
        avg_cycles_group3 = np.mean([test_avg_cycles[test] for test in group3])
        plot_group(ax3, group3, f'High Cycle Count (avg: {avg_cycles_group3:.0f})')
    
    plt.suptitle('Individual Test Total Cycle Counts by Cache Size (Grouped by Cycle Count)', 
                 fontsize=14, y=0.98)
    plt.tight_layout()
    
    # Save the individual test plot
    output_path = os.path.join(output_dir, 'individual_test_cycle_counts.png')
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    print(f"Individual test cycle plot saved to: {output_path}")
    
    plt.show()


def main():
    parser = argparse.ArgumentParser(description='Analyze and plot cycle statistics across different cache sizes')
    parser.add_argument('--base-dir', default='./runs', 
                       help='Base directory containing cache size subdirectories (default: ./runs)')
    parser.add_argument('--output-dir', default='.', 
                       help='Output directory for plots and CSV (default: current directory)')
    
    args = parser.parse_args()
    
    print("Collecting cycle data from log files...")
    data = collect_cycle_data_from_logs(args.base_dir)
    
    if not data:
        print("No data collected. Please check the directory structure and log files.")
        return
    
    print("\nCalculating cycle averages...")
    avg_cycles, avg_total_cycles = calculate_cycle_averages(data)
    
    print("\nCreating cycle plots...")
    create_cycle_plots(avg_cycles, avg_total_cycles, data, args.output_dir)
    
    print("\nCreating individual test cycle plot...")
    create_individual_test_cycle_plot(data, args.output_dir)
    
    print("\nExporting summary CSV...")
    export_summary_csv(avg_cycles, avg_total_cycles, args.output_dir)
    
    print("\nExporting individual cache size CSVs...")
    export_individual_cache_csvs(data, args.output_dir)
    
    print("\nDone!")


if __name__ == "__main__":
    main()
