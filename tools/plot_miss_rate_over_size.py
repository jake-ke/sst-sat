#!/usr/bin/env python3
"""
Script to analyze and plot L1 cache miss rates across different cache sizes.

This script:
1. Parses log files from different cache size directories using unified parser
2. Extracts miss rate data for each data structure from "L1 Cache Profiler Statistics"
3. Calculates averages across all test cases for each cache size
4. Creates two plots:
   - Plot 1: Stacked contributions of each data structure to total miss rate
   - Plot 2: Individual data structure miss rates over cache size
"""

import os
import numpy as np
import matplotlib.pyplot as plt
from collections import defaultdict
import argparse
from unified_parser import parse_log_directory, get_cache_size_from_directory, format_bytes


def collect_data_from_logs(base_dir):
    """
    Collect miss rate data from all cache size directories.
    Returns dict: {cache_size: parsed_data_list}
    """
    cache_dirs = [
        # 'logs_4KiB', 'logs_8KiB', 'logs_16KiB', 'logs_32KiB', 'logs_64KiB', 'logs_128KiB',
        # 'logs_256KiB', 'logs_512KiB', 'logs_1MiB', 'logs_2MiB', 'logs_4MiB'
        'logs_ddr_4KiB', 'logs_ddr_8KiB', 'logs_ddr_16KiB', 'logs_ddr_32KiB', 'logs_ddr_64KiB', 'logs_ddr_128KiB',
        'logs_ddr_256KiB', 'logs_ddr_512KiB', 'logs_ddr_1MiB', 'logs_ddr_2MiB', 'logs_ddr_4MiB'
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
        
        data[cache_size] = results
        print(f"  Processed {len(results)} files")
    
    return data


def calculate_averages(data):
    """
    Calculate average miss rates and contributions for each cache size.
    Returns:
    - avg_miss_rates: {cache_size: {data_structure: avg_miss_rate}}
    - avg_contributions: {cache_size: {data_structure: avg_contribution_to_total}}
    - avg_total_l1_miss_rates: {cache_size: avg_actual_l1_miss_rate}
    """
    avg_miss_rates = {}
    avg_contributions = {}
    avg_total_l1_miss_rates = {}
    
    for cache_size, results in data.items():
        avg_miss_rates[cache_size] = {}
        avg_contributions[cache_size] = {}
        
        # Filter results with L1 cache data
        l1_results = [r for r in results if r.get('l1_total_requests', 0) > 0]
        
        if not l1_results:
            continue
            
        # Calculate average total L1 miss rate
        total_miss_rates = [r['l1_total_miss_rate'] for r in l1_results]
        avg_total_l1_miss_rates[cache_size] = np.mean(total_miss_rates)
        
        # For each data structure, calculate averages
        components = ['heap', 'variables', 'watches', 'clauses', 'varactivity']
        
        for comp in components:
            miss_rate_key = f'l1_{comp}_miss_rate'
            misses_key = f'l1_{comp}_misses'
            
            # Calculate average miss rate
            comp_results = [r for r in l1_results if r.get(miss_rate_key) is not None]
            if comp_results:
                miss_rates = [r[miss_rate_key] for r in comp_results]
                avg_miss_rates[cache_size][comp] = np.mean(miss_rates)
                
                # Calculate average contribution to total miss rate
                contributions = []
                for r in comp_results:
                    if r.get(misses_key) is not None and r.get('l1_total_requests', 0) > 0:
                        contribution = (r[misses_key] / r['l1_total_requests']) * 100
                        contributions.append(contribution)
                
                if contributions:
                    avg_contributions[cache_size][comp] = np.mean(contributions)
                else:
                    avg_contributions[cache_size][comp] = 0.0
    
    return avg_miss_rates, avg_contributions, avg_total_l1_miss_rates


def create_plots(avg_miss_rates, avg_contributions, avg_total_l1_miss_rates, output_dir='.'):
    """Create two plots as requested."""
    
    # Get sorted cache sizes and data structure names
    cache_sizes = sorted(avg_miss_rates.keys())
    
    # Get all data structure names (excluding TOTAL and ClaActivity which is usually 0)
    all_ds_names = set()
    for cache_data in avg_miss_rates.values():
        all_ds_names.update(cache_data.keys())
    
    # Remove TOTAL and filter out data structures that are always zero
    all_ds_names.discard('TOTAL')
    
    # Filter out data structures that have very low contributions across all cache sizes
    filtered_ds_names = []
    for ds_name in all_ds_names:
        max_contribution = max([avg_contributions[cs].get(ds_name, 0) for cs in cache_sizes])
        if max_contribution > 0.01:  # Only include if contribution > 0.01% somewhere
            filtered_ds_names.append(ds_name)
    
    filtered_ds_names = sorted(filtered_ds_names)
    
    # Convert cache sizes to more readable format
    cache_size_labels = []
    for size in cache_sizes:
        if size >= 1024*1024:
            cache_size_labels.append(f"{size//(1024*1024)}MiB")
        elif size >= 1024:
            cache_size_labels.append(f"{size//1024}KiB")
        else:
            cache_size_labels.append(f"{size}B")
    
    # Nice colors for different data structures
    nice_colors = [
        '#2E86AB',  # Blue
        '#A23B72',  # Purple
        '#F18F01',  # Orange
        '#C73E1D',  # Red
        '#4CAF50',  # Green
        '#FF9800',  # Amber
        '#9C27B0',  # Purple variant
        '#607D8B'   # Blue grey
    ]
    
    # Ensure we have enough colors
    while len(nice_colors) < len(filtered_ds_names):
        nice_colors.extend(nice_colors)
    
    color_map = dict(zip(filtered_ds_names, nice_colors[:len(filtered_ds_names)]))
    
    # Plot 1: Stacked area chart showing L1 miss rate breakdown
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(12, 10))
    
    # Prepare data for stacking
    contributions_matrix = np.zeros((len(filtered_ds_names), len(cache_sizes)))
    for i, ds_name in enumerate(filtered_ds_names):
        for j, cache_size in enumerate(cache_sizes):
            contributions_matrix[i, j] = avg_contributions[cache_size].get(ds_name, 0)
    
    # Create stacked area chart
    ax1.stackplot(cache_size_labels, *contributions_matrix, 
                  labels=filtered_ds_names, 
                  colors=[color_map[ds] for ds in filtered_ds_names],
                  alpha=0.8)
    
    # Plot total L1 miss rate line on top
    total_miss_rates = [avg_total_l1_miss_rates.get(cs, 0) for cs in cache_sizes]
    ax1.plot(cache_size_labels, total_miss_rates, marker='s', linewidth=3, 
            label='Total L1 Miss Rate', color='black', markersize=8, linestyle='--')
    
    ax1.set_xlabel('Cache Size')
    ax1.set_ylabel('Miss Rate (%)')
    ax1.set_title('L1 Cache Miss Rate Breakdown by Data Structure and Cache Size (Stacked)')
    ax1.legend(loc='upper right')
    ax1.grid(True, alpha=0.3)
    
    # Plot 2: Individual data structure miss rates (without log scale)
    for ds_name in filtered_ds_names:
        miss_rates = [avg_miss_rates[cs].get(ds_name, 0) for cs in cache_sizes]
        ax2.plot(cache_size_labels, miss_rates, marker='o', linewidth=2, 
                label=ds_name, color=color_map[ds_name])
    
    ax2.set_xlabel('Cache Size')
    ax2.set_ylabel('Average Miss Rate (%)')
    ax2.set_title('Individual Data Structure L1 Cache Miss Rates by Cache Size')
    ax2.legend(loc='upper right')
    ax2.grid(True, alpha=0.3)
    
    # Rotate x-axis labels for better readability
    for ax in [ax1, ax2]:
        ax.tick_params(axis='x', rotation=45)
    
    plt.tight_layout()
    
    # Save plots
    output_path = os.path.join(output_dir, 'l1_miss_rates_by_cache_size.png')
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    print(f"Plots saved to: {output_path}")
    
    # Also save data to CSV for reference
    csv_path = os.path.join(output_dir, 'l1_miss_rates_summary.csv')
    with open(csv_path, 'w') as f:
        f.write('Cache_Size,Total_L1_Miss_Rate(%),')
        f.write(','.join([f'{ds}_Miss_Rate(%)' for ds in filtered_ds_names]))
        f.write(',')
        f.write(','.join([f'{ds}_Contribution(%)' for ds in filtered_ds_names]))
        f.write('\n')
        
        for i, cache_size in enumerate(cache_sizes):
            f.write(f'{cache_size_labels[i]},')
            f.write(f'{avg_total_l1_miss_rates.get(cache_size, 0)},')
            miss_rates = [str(avg_miss_rates[cache_size].get(ds, 0)) for ds in filtered_ds_names]
            contributions = [str(avg_contributions[cache_size].get(ds, 0)) for ds in filtered_ds_names]
            f.write(','.join(miss_rates))
            f.write(',')
            f.write(','.join(contributions))
            f.write('\n')
    
    print(f"Data summary saved to: {csv_path}")
    
    plt.show()


def create_individual_test_plot(data, output_dir='.'):
    """Create a plot showing each individual test's L1 miss rate over cache size, grouped by total requests."""
    
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
    
    # Collect all test miss rates and total requests for each cache size
    all_test_names = set()
    test_data = {}  # {test_name: {cache_size: {'miss_rate': X, 'total_requests': Y}}}
    
    # First pass: collect all unique test names and their data
    for cache_size, results in data.items():
        l1_results = [r for r in results if r.get('l1_total_requests', 0) > 0]
        
        for result in l1_results:
            test_name = result['test_case']
            all_test_names.add(test_name)
            
            if test_name not in test_data:
                test_data[test_name] = {}
            
            test_data[test_name][cache_size] = {
                'miss_rate': result['l1_total_miss_rate'],
                'total_requests': result['l1_total_requests']
            }
    
    # Calculate average total requests for each test across all cache sizes
    test_avg_requests = {}
    for test_name in all_test_names:
        total_requests_list = []
        for cache_size in cache_sizes:
            if cache_size in test_data[test_name]:
                total_requests_list.append(test_data[test_name][cache_size]['total_requests'])
        
        if total_requests_list:
            test_avg_requests[test_name] = np.mean(total_requests_list)
    
    # Sort tests by average total requests and divide into 3 groups
    sorted_tests = sorted(test_avg_requests.items(), key=lambda x: x[1])
    total_tests = len(sorted_tests)
    
    group_size = total_tests // 3
    group1 = [test[0] for test in sorted_tests[:group_size]]  # Low requests
    group2 = [test[0] for test in sorted_tests[group_size:2*group_size]]  # Medium requests
    group3 = [test[0] for test in sorted_tests[2*group_size:]]  # High requests
    
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
            miss_rates = []
            valid_labels = []
            
            for j, cache_size in enumerate(cache_sizes):
                if cache_size in test_data[test_name]:
                    miss_rates.append(test_data[test_name][cache_size]['miss_rate'])
                    valid_labels.append(cache_size_labels[j])
            
            if len(miss_rates) > 1:  # Only plot if we have data for multiple cache sizes
                color = diverse_colors[i % len(diverse_colors)]
                ax.plot(valid_labels, miss_rates, linewidth=2.5, alpha=0.8, marker='o', 
                       color=color, markersize=5)
        
        ax.set_xlabel('Cache Size')
        ax.set_ylabel('L1 Miss Rate (%)')
        ax.set_title(f'{group_name} ({len(group_tests)} tests)')
        ax.set_ylim(-5, 55)  # Set consistent y-axis range for all subplots
        ax.grid(True, alpha=0.3)
        ax.tick_params(axis='x', rotation=45)
    
    # Plot each group
    if group1:
        avg_req_group1 = np.mean([test_avg_requests[test] for test in group1])
        plot_group(ax1, group1, f'Low Total Requests (avg: {avg_req_group1:.0f})')
    
    if group2:
        avg_req_group2 = np.mean([test_avg_requests[test] for test in group2])
        plot_group(ax2, group2, f'Medium Total Requests (avg: {avg_req_group2:.0f})')
    
    if group3:
        avg_req_group3 = np.mean([test_avg_requests[test] for test in group3])
        plot_group(ax3, group3, f'High Total Requests (avg: {avg_req_group3:.0f})')
    
    plt.suptitle('Individual Test L1 Cache Miss Rates by Cache Size (Grouped by Total Requests)', 
                 fontsize=14, y=0.98)
    plt.tight_layout()
    
    # Save the individual test plot
    output_path = os.path.join(output_dir, 'individual_test_l1_miss_rates.png')
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    print(f"Individual test plot saved to: {output_path}")
    
    plt.show()


def create_memory_clustered_plot(data, output_dir='.'):
    """Create a plot showing each individual test's L1 miss rate over cache size, grouped by memory usage."""
    
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
    
    # Collect all test miss rates and memory usage for each cache size
    all_test_names = set()
    test_data = {}  # {test_name: {cache_size: {'miss_rate': X, 'memory_bytes': Y}}}
    
    # First pass: collect all unique test names and their data
    for cache_size, results in data.items():
        l1_results = [r for r in results if r.get('l1_total_requests', 0) > 0]
        
        for result in l1_results:
            test_name = result['test_case']
            all_test_names.add(test_name)
            
            if test_name not in test_data:
                test_data[test_name] = {}
            
            test_data[test_name][cache_size] = {
                'miss_rate': result['l1_total_miss_rate'],
                'memory_bytes': result.get('total_memory_bytes', 0)
            }
    
    # Calculate average memory usage for each test across all cache sizes
    test_avg_memory = {}
    for test_name in all_test_names:
        memory_list = []
        for cache_size in cache_sizes:
            if cache_size in test_data[test_name]:
                memory_list.append(test_data[test_name][cache_size]['memory_bytes'])
        
        if memory_list:
            test_avg_memory[test_name] = np.mean(memory_list)
    
    # Sort tests by average memory usage and divide into 3 groups
    sorted_tests = sorted(test_avg_memory.items(), key=lambda x: x[1])
    total_tests = len(sorted_tests)
    
    group_size = total_tests // 3
    group1 = [test[0] for test in sorted_tests[:group_size]]  # Low memory
    group2 = [test[0] for test in sorted_tests[group_size:2*group_size]]  # Medium memory
    group3 = [test[0] for test in sorted_tests[2*group_size:]]  # High memory
    
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
            miss_rates = []
            valid_labels = []
            
            for j, cache_size in enumerate(cache_sizes):
                if cache_size in test_data[test_name]:
                    miss_rates.append(test_data[test_name][cache_size]['miss_rate'])
                    valid_labels.append(cache_size_labels[j])
            
            if len(miss_rates) > 1:  # Only plot if we have data for multiple cache sizes
                color = diverse_colors[i % len(diverse_colors)]
                ax.plot(valid_labels, miss_rates, linewidth=2.5, alpha=0.8, marker='o', 
                       color=color, markersize=5)
        
        ax.set_xlabel('Cache Size')
        ax.set_ylabel('L1 Miss Rate (%)')
        ax.set_title(f'{group_name} ({len(group_tests)} tests)')
        ax.set_ylim(-5, 55)  # Set consistent y-axis range for all subplots
        ax.grid(True, alpha=0.3)
        ax.tick_params(axis='x', rotation=45)
    
    # Plot each group with memory information
    if group1:
        avg_memory_group1 = np.mean([test_avg_memory[test] for test in group1])
        memory_str1 = format_bytes(avg_memory_group1) if avg_memory_group1 > 0 else "0 B"
        plot_group(ax1, group1, f'Low Memory Usage (avg: {memory_str1})')
    
    if group2:
        avg_memory_group2 = np.mean([test_avg_memory[test] for test in group2])
        memory_str2 = format_bytes(avg_memory_group2) if avg_memory_group2 > 0 else "0 B"
        plot_group(ax2, group2, f'Medium Memory Usage (avg: {memory_str2})')
    
    if group3:
        avg_memory_group3 = np.mean([test_avg_memory[test] for test in group3])
        memory_str3 = format_bytes(avg_memory_group3) if avg_memory_group3 > 0 else "0 B"
        plot_group(ax3, group3, f'High Memory Usage (avg: {memory_str3})')
    
    plt.suptitle('Individual Test L1 Cache Miss Rates by Cache Size (Grouped by Memory Usage)', 
                 fontsize=14, y=0.98)
    plt.tight_layout()
    
    # Save the memory-clustered plot
    output_path = os.path.join(output_dir, 'memory_clustered_l1_miss_rates.png')
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    print(f"Memory-clustered plot saved to: {output_path}")
    
    plt.show()


def main():
    parser = argparse.ArgumentParser(description='Plot L1 cache miss rates across different cache sizes')
    parser.add_argument('--base-dir', default='./runs', 
                       help='Base directory containing cache size subdirectories (default: ./runs)')
    parser.add_argument('--output-dir', default='.', 
                       help='Output directory for plots and CSV (default: current directory)')
    
    args = parser.parse_args()
    
    print("Collecting data from log files...")
    data = collect_data_from_logs(args.base_dir)
    
    if not data:
        print("No data collected. Please check the directory structure and log files.")
        return
    
    print("\nCalculating averages...")
    avg_miss_rates, avg_contributions, avg_total_l1_miss_rates = calculate_averages(data)
    
    print("\nCreating plots...")
    create_plots(avg_miss_rates, avg_contributions, avg_total_l1_miss_rates, args.output_dir)
    
    print("\nCreating individual test plot...")
    create_individual_test_plot(data, args.output_dir)

    print("\nCreating memory-clustered plot...")
    create_memory_clustered_plot(data, args.output_dir)
    
    print("\nDone!")


if __name__ == "__main__":
    main()
