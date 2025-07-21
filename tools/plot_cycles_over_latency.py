#!/usr/bin/env python3
"""
Script to analyze and plot cycle statistics across different L1 cache latencies.

This script:
1. Parses log files from different L1 cache latency directories using unified parser
2. Extracts cycle statistics from "Cycle Statistics" section
3. Calculates averages across all test cases for each latency
4. Creates two plots:
   - Plot 1: Stacked area chart showing cycle breakdown by latency
   - Plot 2: Individual cycle stage percentages over latency
   - Plot 3: Geometric mean of total cycle counts by latency
5. Exports CSV with cycle data by latency
"""

import os
import numpy as np
import matplotlib.pyplot as plt
import csv
import argparse
from collections import defaultdict
from unified_parser import parse_log_directory


def get_latency_from_directory(directory_name):
    """Extract L1 cache latency from directory name like 'logs_l1_10ns_mem_100ns'."""
    import re
    latency_match = re.search(r'logs_l1_(\d+)ns_mem_\d+ns', directory_name)
    if not latency_match:
        return None
    
    return int(latency_match.group(1))


def get_memory_latency_from_directory(directory_name):
    """Extract main memory latency from directory name like 'logs_l1_1ns_mem_60ns'."""
    import re
    latency_match = re.search(r'logs_l1_1ns_mem_(\d+)ns', directory_name)
    if not latency_match:
        return None
    
    return int(latency_match.group(1))


def collect_cycle_data_from_logs(base_dir):
    """
    Collect cycle data from all L1 cache latency directories.
    Returns dict: {latency_ns: parsed_data_list}
    """
    latency_dirs = [
        'logs_l1_1ns_mem_100ns',
        'logs_l1_5ns_mem_100ns', 
        'logs_l1_10ns_mem_100ns',
        'logs_l1_30ns_mem_100ns',
        'logs_l1_50ns_mem_100ns',
        'logs_l1_100ns_mem_100ns'
    ]
    
    data = {}
    
    for latency_dir in latency_dirs:
        latency_path = os.path.join(base_dir, latency_dir)
        if not os.path.exists(latency_path):
            print(f"Warning: Directory {latency_path} not found")
            continue
        
        latency_ns = get_latency_from_directory(latency_dir)
        if latency_ns is None:
            print(f"Warning: Could not parse latency from {latency_dir}")
            continue
        
        print(f"Processing {latency_dir} (L1 latency: {latency_ns} ns)...")
        
        # Parse all log files using unified parser
        results = parse_log_directory(latency_path, exclude_summary=True)
        
        if not results:
            print(f"Warning: No valid log files found in {latency_path}")
            continue
        
        # Filter results that have cycle data
        cycle_results = [r for r in results if r.get('total_counted_cycles', 0) > 0]
        
        if not cycle_results:
            print(f"Warning: No cycle data found in {latency_path}")
            continue
        
        data[latency_ns] = cycle_results
        print(f"  Processed {len(cycle_results)} files with cycle data")
    
    return data


def collect_memory_cycle_data_from_logs(base_dir):
    """
    Collect cycle data from all main memory latency directories.
    Returns dict: {memory_latency_ns: parsed_data_list}
    """
    memory_dirs = [
        'logs_l1_1ns_mem_50ns',
        'logs_l1_1ns_mem_60ns', 
        'logs_l1_1ns_mem_70ns', 
        'logs_l1_1ns_mem_80ns', 
        'logs_l1_1ns_mem_90ns', 
        'logs_l1_1ns_mem_100ns'
    ]
    
    data = {}
    
    for memory_dir in memory_dirs:
        memory_path = os.path.join(base_dir, memory_dir)
        if not os.path.exists(memory_path):
            print(f"Warning: Directory {memory_path} not found")
            continue
        
        memory_latency_ns = get_memory_latency_from_directory(memory_dir)
        if memory_latency_ns is None:
            print(f"Warning: Could not parse memory latency from {memory_dir}")
            continue
        
        print(f"Processing {memory_dir} (Memory latency: {memory_latency_ns} ns)...")
        
        # Parse all log files using unified parser
        results = parse_log_directory(memory_path, exclude_summary=True)
        
        if not results:
            print(f"Warning: No valid log files found in {memory_path}")
            continue
        
        # Filter results that have cycle data
        cycle_results = [r for r in results if r.get('total_counted_cycles', 0) > 0]
        
        if not cycle_results:
            print(f"Warning: No cycle data found in {memory_path}")
            continue
        
        data[memory_latency_ns] = cycle_results
        print(f"  Processed {len(cycle_results)} files with cycle data")
    
    return data


def calculate_cycle_averages(data):
    """
    Calculate average cycle counts for each latency.
    Returns:
    - avg_cycles: {latency_ns: {cycle_type: avg_cycles}}
    - avg_total_cycles: {latency_ns: avg_total_cycles}
    """
    avg_cycles = {}
    avg_total_cycles = {}
    
    # Cycle types to analyze
    cycle_types = [
        'propagate_cycles', 'analyze_cycles', 'minimize_cycles', 
        'backtrack_cycles', 'decision_cycles', 'reduce_db_cycles', 'restart_cycles'
    ]
    
    for latency_ns, results in data.items():
        avg_cycles[latency_ns] = {}
        
        # Calculate average total cycles
        total_cycles = [r.get('total_counted_cycles', 0) for r in results]
        avg_total_cycles[latency_ns] = np.mean(total_cycles)
        
        # Calculate average for each cycle type
        for cycle_type in cycle_types:
            cycle_counts = [r.get(cycle_type, 0) for r in results]
            avg_cycles[latency_ns][cycle_type] = np.mean(cycle_counts)
    
    return avg_cycles, avg_total_cycles


def create_cycle_plots(avg_cycles, avg_total_cycles, data, output_dir='.'):
    """Create stacked line charts showing cycle breakdown by L1 cache latency."""
    
    # Get sorted latencies
    latencies = sorted(avg_cycles.keys())
    
    # Use actual latency values for linear scale
    latency_values = latencies  # Keep as numbers for linear scale
    
    # Convert latencies to labels
    latency_labels = [f"{lat}ns" for lat in latencies]
    
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
    cycle_data = np.zeros((len(cycle_types), len(latencies)))
    for i, cycle_type in enumerate(cycle_types):
        for j, latency_ns in enumerate(latencies):
            cycle_data[i, j] = avg_cycles[latency_ns].get(cycle_type, 0)
    
    ax1.stackplot(latency_values, *cycle_data, 
                  labels=cycle_labels, colors=colors, alpha=0.8)
    
    # Plot total cycles line on top
    total_cycles = [avg_total_cycles.get(lat, 0) for lat in latencies]
    ax1.plot(latency_values, total_cycles, marker='s', linewidth=3, 
            label='Total Cycles', color='black', markersize=8, linestyle='--')
    
    ax1.set_xlabel('L1 Cache Latency (ns)')
    ax1.set_ylabel('Average Cycles')
    ax1.set_title('Average Cycle Breakdown by L1 Cache Latency (Absolute)')
    ax1.legend(loc='upper left', bbox_to_anchor=(1.02, 1))
    ax1.grid(True, alpha=0.3)
    
    # Plot 2: Individual cycle stage percentages as line plots
    for i, (cycle_type, cycle_label) in enumerate(zip(cycle_types, cycle_labels)):
        percentages = []
        for latency_ns in latencies:
            total = avg_total_cycles.get(latency_ns, 1)  # Avoid division by zero
            cycle_count = avg_cycles[latency_ns].get(cycle_type, 0)
            percentage = (cycle_count / total) * 100 if total > 0 else 0
            percentages.append(percentage)
        
        ax2.plot(latency_values, percentages, marker='o', linewidth=2, 
                label=cycle_label, color=colors[i])
    
    ax2.set_xlabel('L1 Cache Latency (ns)')
    ax2.set_ylabel('Percentage of Total Cycles (%)')
    ax2.set_title('Individual Cycle Stage Percentages by L1 Cache Latency')
    ax2.legend(loc='upper left', bbox_to_anchor=(1.02, 1))
    ax2.grid(True, alpha=0.3)
    ax2.set_ylim(0, max([max([
        (avg_cycles[lat].get(ct, 0) / avg_total_cycles.get(lat, 1)) * 100 
        for lat in latencies
    ]) for ct in cycle_types]) + 5)  # Set y-limit based on max percentage + 5%
    
    # Plot 3: Geometric mean of total cycle counts across latencies
    geomean_cycles = []
    for latency_ns in latencies:
        # Get all cycle counts for this latency from the data
        cache_results = []
        for lat, results in data.items():
            if lat == latency_ns:
                cycle_counts = [r.get('total_counted_cycles', 0) for r in results if r.get('total_counted_cycles', 0) > 0]
                cache_results = cycle_counts
                break
        
        if cache_results:
            # Calculate geometric mean (using numpy's implementation)
            geomean = np.exp(np.mean(np.log(cache_results)))
            geomean_cycles.append(geomean)
        else:
            geomean_cycles.append(0)
    
    ax3.plot(latency_values, geomean_cycles, marker='o', linewidth=3, 
            color='#2E86AB', markersize=8)
    
    ax3.set_xlabel('L1 Cache Latency (ns)')
    ax3.set_ylabel('Geometric Mean Total Cycles')
    ax3.set_title('Geometric Mean of Total Cycle Counts by L1 Cache Latency')
    ax3.grid(True, alpha=0.3)
    
    plt.tight_layout()
    
    # Save plots
    output_path = os.path.join(output_dir, 'cycle_breakdown_by_l1_latency.png')
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    print(f"Cycle plots saved to: {output_path}")
    
    plt.show()


def create_memory_cycle_plots(avg_cycles, avg_total_cycles, data, output_dir='.'):
    """Create stacked line charts showing cycle breakdown by main memory latency."""
    
    # Get sorted latencies
    latencies = sorted(avg_cycles.keys())
    
    # Use actual latency values for linear scale
    latency_values = latencies  # Keep as numbers for linear scale
    
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
    cycle_data = np.zeros((len(cycle_types), len(latencies)))
    for i, cycle_type in enumerate(cycle_types):
        for j, latency_ns in enumerate(latencies):
            cycle_data[i, j] = avg_cycles[latency_ns].get(cycle_type, 0)
    
    ax1.stackplot(latency_values, *cycle_data, 
                  labels=cycle_labels, colors=colors, alpha=0.8)
    
    # Plot total cycles line on top
    total_cycles = [avg_total_cycles.get(lat, 0) for lat in latencies]
    ax1.plot(latency_values, total_cycles, marker='s', linewidth=3, 
            label='Total Cycles', color='black', markersize=8, linestyle='--')
    
    ax1.set_xlabel('Main Memory Latency (ns)')
    ax1.set_ylabel('Average Cycles')
    ax1.set_title('Average Cycle Breakdown by Main Memory Latency (Absolute)')
    ax1.legend(loc='upper left', bbox_to_anchor=(1.02, 1))
    ax1.grid(True, alpha=0.3)
    
    # Plot 2: Individual cycle stage percentages as line plots
    for i, (cycle_type, cycle_label) in enumerate(zip(cycle_types, cycle_labels)):
        percentages = []
        for latency_ns in latencies:
            total = avg_total_cycles.get(latency_ns, 1)  # Avoid division by zero
            cycle_count = avg_cycles[latency_ns].get(cycle_type, 0)
            percentage = (cycle_count / total) * 100 if total > 0 else 0
            percentages.append(percentage)
        
        ax2.plot(latency_values, percentages, marker='o', linewidth=2, 
                label=cycle_label, color=colors[i])
    
    ax2.set_xlabel('Main Memory Latency (ns)')
    ax2.set_ylabel('Percentage of Total Cycles (%)')
    ax2.set_title('Individual Cycle Stage Percentages by Main Memory Latency')
    ax2.legend(loc='upper left', bbox_to_anchor=(1.02, 1))
    ax2.grid(True, alpha=0.3)
    ax2.set_ylim(0, max([max([
        (avg_cycles[lat].get(ct, 0) / avg_total_cycles.get(lat, 1)) * 100 
        for lat in latencies
    ]) for ct in cycle_types]) + 5)  # Set y-limit based on max percentage + 5%
    
    # Plot 3: Geometric mean of total cycle counts across latencies
    geomean_cycles = []
    for latency_ns in latencies:
        # Get all cycle counts for this latency from the data
        cache_results = []
        for lat, results in data.items():
            if lat == latency_ns:
                cycle_counts = [r.get('total_counted_cycles', 0) for r in results if r.get('total_counted_cycles', 0) > 0]
                cache_results = cycle_counts
                break
        
        if cache_results:
            # Calculate geometric mean (using numpy's implementation)
            geomean = np.exp(np.mean(np.log(cache_results)))
            geomean_cycles.append(geomean)
        else:
            geomean_cycles.append(0)
    
    ax3.plot(latency_values, geomean_cycles, marker='o', linewidth=3, 
            color='#2E86AB', markersize=8)
    
    ax3.set_xlabel('Main Memory Latency (ns)')
    ax3.set_ylabel('Geometric Mean Total Cycles')
    ax3.set_title('Geometric Mean of Total Cycle Counts by Main Memory Latency')
    ax3.grid(True, alpha=0.3)
    
    plt.tight_layout()
    
    # Save plots
    output_path = os.path.join(output_dir, 'cycle_breakdown_by_memory_latency.png')
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    print(f"Memory cycle plots saved to: {output_path}")
    
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


def create_individual_test_cycle_plot(data, output_dir='.'):
    """Create a plot showing each individual test's total cycle count over L1 latency, grouped by total cycle count."""
    
    # Get sorted latencies
    latencies = sorted(data.keys())
    
    # Use actual latency values for linear scale
    latency_values = latencies  # Keep as numbers for linear scale
    
    # Collect all test cycle counts for each latency
    all_test_names = set()
    test_data = {}  # {test_name: {latency_ns: total_cycles}}
    
    # First pass: collect all unique test names and their data
    for latency_ns, results in data.items():
        cycle_results = [r for r in results if r.get('total_counted_cycles', 0) > 0]
        
        for result in cycle_results:
            test_name = result['test_case']
            all_test_names.add(test_name)
            
            if test_name not in test_data:
                test_data[test_name] = {}
            
            test_data[test_name][latency_ns] = result['total_counted_cycles']
    
    # Calculate average total cycles for each test across all latencies
    test_avg_cycles = {}
    for test_name in all_test_names:
        cycle_counts = []
        for latency_ns in latencies:
            if latency_ns in test_data[test_name]:
                cycle_counts.append(test_data[test_name][latency_ns])
        
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
            valid_latencies = []
            
            for latency_ns in latencies:
                if latency_ns in test_data[test_name]:
                    cycle_counts.append(test_data[test_name][latency_ns])
                    valid_latencies.append(latency_ns)
            
            if len(cycle_counts) > 1:  # Only plot if we have data for multiple latencies
                color = diverse_colors[i % len(diverse_colors)]
                ax.plot(valid_latencies, cycle_counts, linewidth=2.5, alpha=0.8, marker='o', 
                       color=color, markersize=5)
        
        ax.set_xlabel('L1 Cache Latency (ns)')
        ax.set_ylabel('Total Cycles')
        ax.set_title(f'{group_name} ({len(group_tests)} tests)')
        ax.grid(True, alpha=0.3)
    
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
    
    plt.suptitle('Individual Test Total Cycle Counts by L1 Cache Latency (Grouped by Cycle Count)', 
                 fontsize=14, y=0.98)
    plt.tight_layout()
    
    # Save the individual test plot
    output_path = os.path.join(output_dir, 'individual_test_cycle_counts_by_latency.png')
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    print(f"Individual test cycle plot saved to: {output_path}")
    
    plt.show()


def create_individual_test_memory_cycle_plot(data, output_dir='.'):
    """Create a plot showing each individual test's total cycle count over main memory latency, grouped by total cycle count."""
    
    # Get sorted latencies
    latencies = sorted(data.keys())
    
    # Use actual latency values for linear scale
    latency_values = latencies  # Keep as numbers for linear scale
    
    # Collect all test cycle counts for each latency
    all_test_names = set()
    test_data = {}  # {test_name: {latency_ns: total_cycles}}
    
    # First pass: collect all unique test names and their data
    for latency_ns, results in data.items():
        cycle_results = [r for r in results if r.get('total_counted_cycles', 0) > 0]
        
        for result in cycle_results:
            test_name = result['test_case']
            all_test_names.add(test_name)
            
            if test_name not in test_data:
                test_data[test_name] = {}
            
            test_data[test_name][latency_ns] = result['total_counted_cycles']
    
    # Calculate average total cycles for each test across all latencies
    test_avg_cycles = {}
    for test_name in all_test_names:
        cycle_counts = []
        for latency_ns in latencies:
            if latency_ns in test_data[test_name]:
                cycle_counts.append(test_data[test_name][latency_ns])
        
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
            valid_latencies = []
            
            for latency_ns in latencies:
                if latency_ns in test_data[test_name]:
                    cycle_counts.append(test_data[test_name][latency_ns])
                    valid_latencies.append(latency_ns)
            
            if len(cycle_counts) > 1:  # Only plot if we have data for multiple latencies
                color = diverse_colors[i % len(diverse_colors)]
                ax.plot(valid_latencies, cycle_counts, linewidth=2.5, alpha=0.8, marker='o', 
                       color=color, markersize=5)
        
        ax.set_xlabel('Main Memory Latency (ns)')
        ax.set_ylabel('Total Cycles')
        ax.set_title(f'{group_name} ({len(group_tests)} tests)')
        ax.grid(True, alpha=0.3)
    
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
    
    plt.suptitle('Individual Test Total Cycle Counts by Main Memory Latency (Grouped by Cycle Count)', 
                 fontsize=14, y=0.98)
    plt.tight_layout()
    
    # Save the individual test plot
    output_path = os.path.join(output_dir, 'individual_test_cycle_counts_by_memory_latency.png')
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    print(f"Individual test memory cycle plot saved to: {output_path}")
    
    plt.show()


def export_summary_csv(avg_cycles, avg_total_cycles, output_dir='.'):
    """Export summary CSV with averages by L1 cache latency."""
    
    csv_path = os.path.join(output_dir, 'cycle_analysis_latency_summary.csv')
    
    # Get sorted latencies
    latencies = sorted(avg_cycles.keys())
    
    # Cycle types
    cycle_types = [
        'propagate_cycles', 'analyze_cycles', 'minimize_cycles', 
        'backtrack_cycles', 'decision_cycles', 'reduce_db_cycles', 'restart_cycles'
    ]
    
    # Define fieldnames
    fieldnames = ['l1_latency_ns', 'total_avg_cycles']
    fieldnames.extend([f'avg_{ct}' for ct in cycle_types])
    fieldnames.extend([f'pct_{ct.replace("_cycles", "")}' for ct in cycle_types])
    
    # Prepare data
    csv_data = []
    
    for latency_ns in latencies:
        total_avg = avg_total_cycles.get(latency_ns, 0)
        
        row = {
            'l1_latency_ns': latency_ns,
            'total_avg_cycles': total_avg
        }
        
        # Add average cycle counts
        for cycle_type in cycle_types:
            avg_cycles_val = avg_cycles[latency_ns].get(cycle_type, 0)
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


def export_individual_latency_csvs(data, output_dir='.'):
    """Export each latency's data to its own CSV file."""
    
    # Define fieldnames for individual latency CSV files
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
        
        # Solver statistics
        'decisions', 'propagations', 'conflicts', 'learned', 'removed',
        'db_reductions', 'minimized', 'restarts'
    ]
    
    for latency_ns, results in data.items():
        # Create CSV filename
        csv_filename = f'cycle_analysis_l1_{latency_ns}ns.csv'
        csv_path = os.path.join(output_dir, csv_filename)
        
        # Collect data for this latency
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
        
        print(f"L1 latency {latency_ns}ns CSV exported to: {csv_path} ({len(csv_data)} records)")


def main():
    parser = argparse.ArgumentParser(description='Analyze and plot cycle statistics across different L1 cache latencies and main memory latencies')
    parser.add_argument('--base-dir', default='./runs', 
                       help='Base directory containing latency subdirectories (default: ./runs)')
    parser.add_argument('--output-dir', default='.', 
                       help='Output directory for plots and CSV (default: current directory)')
    
    args = parser.parse_args()
    
    print("Collecting L1 cache cycle data from log files...")
    l1_data = collect_cycle_data_from_logs(args.base_dir)
    
    if l1_data:
        print("\nCalculating L1 cache cycle averages...")
        l1_avg_cycles, l1_avg_total_cycles = calculate_cycle_averages(l1_data)
        
        print("\nCreating L1 cache cycle plots...")
        create_cycle_plots(l1_avg_cycles, l1_avg_total_cycles, l1_data, args.output_dir)
        
        print("\nCreating individual test L1 cache cycle plot...")
        create_individual_test_cycle_plot(l1_data, args.output_dir)
        
        print("\nExporting L1 cache summary CSV...")
        export_summary_csv(l1_avg_cycles, l1_avg_total_cycles, args.output_dir)
        
        print("\nExporting individual L1 latency CSVs...")
        export_individual_latency_csvs(l1_data, args.output_dir)
    else:
        print("No L1 cache data collected.")
    
    print("\n" + "="*60)
    print("Collecting main memory cycle data from log files...")
    memory_data = collect_memory_cycle_data_from_logs(args.base_dir)
    
    if memory_data:
        print("\nCalculating main memory cycle averages...")
        memory_avg_cycles, memory_avg_total_cycles = calculate_cycle_averages(memory_data)
        
        print("\nCreating main memory cycle plots...")
        create_memory_cycle_plots(memory_avg_cycles, memory_avg_total_cycles, memory_data, args.output_dir)
        
        print("\nCreating individual test main memory cycle plot...")
        create_individual_test_memory_cycle_plot(memory_data, args.output_dir)
    else:
        print("No main memory data collected.")
    
    print("\nDone!")


if __name__ == "__main__":
    main()
