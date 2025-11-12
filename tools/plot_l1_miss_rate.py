#!/usr/bin/env python3
"""
L1 Cache Miss Rate Plotter

This script plots L1 cache miss rate trends over different cache sizes with
breakdown by data structure.

The input takes:
1. A directory containing 4 cache size folders (base_64KB, base_128KB, base_256KB, base_512KB)
2. A base folder name (e.g., base_l1_4_1_l2_8_32) that exists under each size folder

Each base folder should contain the same number of seed folders (seed0, seed1, etc.)
Only finished tests (SAT/UNSAT) are included in the averaging.

Data structures shown:
- Priority Queue (sum of heap and varactivity)
- Clauses
- Variables
- Watchlist

Usage: python plot_l1_miss_rate.py <cache_sizes_dir> <base_folder_name> [output.pdf]
"""

import sys
from pathlib import Path
import matplotlib.pyplot as plt
import numpy as np
from unified_parser import parse_log_directory


def extract_cache_size_kb(folder_name):
    """Extract cache size in KB from folder name like 'base_64KB' or 'base_512KB'."""
    import re
    match = re.search(r'(\d+)KB', folder_name)
    if match:
        return int(match.group(1))
    return None


def compute_l1_miss_rates(results):
    """Compute L1 miss rates across finished tests.
    
    Returns dict with:
    - total: overall miss rate
    - priority_queue: contribution of priority queue misses to total miss rate
    - clauses: contribution of clauses misses to total miss rate
    - variables: contribution of variables misses to total miss rate
    - watchlist: contribution of watchlist misses to total miss rate
    
    The data structure contributions are computed as:
    (data_structure_misses / total_requests) * 100.0
    
    So they add up to the total miss rate.
    """
    # Filter to finished tests only
    finished = [r for r in results if r.get('result') in ('SAT', 'UNSAT')]
    
    if not finished:
        return {}
    
    # Aggregate actual miss and request counts
    total_requests = 0
    total_hits = 0
    total_misses = 0
    
    heap_requests = 0
    heap_hits = 0
    heap_misses = 0
    
    varactivity_requests = 0
    varactivity_hits = 0
    varactivity_misses = 0
    
    clauses_requests = 0
    clauses_hits = 0
    clauses_misses = 0
    
    variables_requests = 0
    variables_hits = 0
    variables_misses = 0
    
    watches_requests = 0
    watches_hits = 0
    watches_misses = 0
    
    for r in finished:
        # Total (use hits + misses for more accurate counts)
        total_req = r.get('l1_total_requests', 0) or 0
        if total_req > 0:
            total_requests += total_req
            # Try to get hits/misses if available, otherwise compute from miss rate
            # Note: total doesn't have separate hits/misses in the parser
            total_miss_rate = r.get('l1_total_miss_rate', 0) or 0
            total_misses += int(total_req * (total_miss_rate / 100.0))
        
        # Heap
        heap_req = r.get('l1_heap_total', 0) or 0
        if heap_req > 0:
            heap_requests += heap_req
            heap_hits += r.get('l1_heap_hits', 0) or 0
            heap_misses += r.get('l1_heap_misses', 0) or 0
        
        # VarActivity
        var_act_req = r.get('l1_varactivity_total', 0) or 0
        if var_act_req > 0:
            varactivity_requests += var_act_req
            varactivity_hits += r.get('l1_varactivity_hits', 0) or 0
            varactivity_misses += r.get('l1_varactivity_misses', 0) or 0
        
        # Clauses
        clauses_req = r.get('l1_clauses_total', 0) or 0
        if clauses_req > 0:
            clauses_requests += clauses_req
            clauses_hits += r.get('l1_clauses_hits', 0) or 0
            clauses_misses += r.get('l1_clauses_misses', 0) or 0
        
        # Variables
        vars_req = r.get('l1_variables_total', 0) or 0
        if vars_req > 0:
            variables_requests += vars_req
            variables_hits += r.get('l1_variables_hits', 0) or 0
            variables_misses += r.get('l1_variables_misses', 0) or 0
        
        # Watches
        watches_req = r.get('l1_watches_total', 0) or 0
        if watches_req > 0:
            watches_requests += watches_req
            watches_hits += r.get('l1_watches_hits', 0) or 0
            watches_misses += r.get('l1_watches_misses', 0) or 0
    
    miss_rates = {}
    
    # Total miss rate
    if total_requests > 0:
        miss_rates['total'] = (total_misses / total_requests) * 100.0
        
        # Compute each data structure's contribution to the total miss rate
        # This is: (data_structure_misses / total_requests) * 100.0
        # So they will add up to the total miss rate
        
        # Priority Queue (combined heap + varactivity)
        pq_misses = heap_misses + varactivity_misses
        miss_rates['priority_queue'] = (pq_misses / total_requests) * 100.0
        
        # Individual data structures
        miss_rates['clauses'] = (clauses_misses / total_requests) * 100.0
        miss_rates['variables'] = (variables_misses / total_requests) * 100.0
        miss_rates['watchlist'] = (watches_misses / total_requests) * 100.0
    
    return miss_rates


def collect_miss_rate_data(cache_sizes_dir, base_folder_name):
    """Collect miss rate data for all cache sizes.
    
    Returns:
    - cache_sizes: list of cache sizes in KB
    - miss_rate_data: dict mapping data structure name to list of miss rates
    """
    cache_sizes_dir = Path(cache_sizes_dir)
    
    if not cache_sizes_dir.exists():
        print(f"Error: Directory {cache_sizes_dir} does not exist")
        return None, None
    
    # Expected cache size folders
    size_folders = ['base_64KB', 'base_128KB', 'base_256KB', 'base_512KB']
    
    cache_sizes = []
    all_results = {}
    
    for size_folder in size_folders:
        size_path = cache_sizes_dir / size_folder / base_folder_name
        
        if not size_path.exists():
            print(f"Warning: {size_path} does not exist, skipping")
            continue
        
        # Extract cache size
        cache_size = extract_cache_size_kb(size_folder)
        if cache_size is None:
            print(f"Warning: Could not extract cache size from {size_folder}, skipping")
            continue
        
        # Find all seed folders
        seed_dirs = sorted([d for d in size_path.glob('seed*') if d.is_dir()])
        
        if not seed_dirs:
            print(f"Warning: No seed folders found in {size_path}, skipping")
            continue
        
        print(f"\nProcessing {size_folder} ({cache_size} KB) with {len(seed_dirs)} seeds")
        
        # Collect results from all seeds
        all_seed_results = []
        for seed_dir in seed_dirs:
            results = parse_log_directory(seed_dir, exclude_summary=True)
            if results:
                all_seed_results.extend(results)
        
        if not all_seed_results:
            print(f"  No valid results found")
            continue
        
        finished = [r for r in all_seed_results if r.get('result') in ('SAT', 'UNSAT')]
        print(f"  Total tests: {len(all_seed_results)}, Finished: {len(finished)}")
        
        if not finished:
            continue
        
        cache_sizes.append(cache_size)
        all_results[cache_size] = all_seed_results
    
    if not cache_sizes:
        print("\nError: No valid data collected from any cache size")
        return None, None
    
    # Sort by cache size
    cache_sizes.sort()
    
    # Compute miss rates for each cache size
    miss_rate_data = {
        'total': [],
        'priority_queue': [],
        'clauses': [],
        'variables': [],
        'watchlist': []
    }
    
    for size in cache_sizes:
        miss_rates = compute_l1_miss_rates(all_results[size])
        
        for key in miss_rate_data.keys():
            miss_rate_data[key].append(miss_rates.get(key, 0))
    
    return cache_sizes, miss_rate_data


def plot_miss_rates(cache_sizes, miss_rate_data, output_pdf):
    """Create a PDF plot showing miss rate trends over cache sizes.
    
    Shows total miss rate as a line, and stacked area chart for breakdown
    by data structure. Legends are ranked by their average miss rate percentage.
    """
    fig, ax = plt.subplots(1, 1, figsize=(10, 6))
    
    # Prepare data for plotting
    data_structures = ['priority_queue', 'clauses', 'variables', 'watchlist']
    labels_map = {
        'priority_queue': 'Priority Queue',
        'clauses': 'Clauses',
        'variables': 'Variables',
        'watchlist': 'Watchlist'
    }
    
    # Calculate average miss rate for each data structure for ranking
    avg_miss_rates = {}
    for ds in data_structures:
        rates = miss_rate_data[ds]
        if rates:
            avg_miss_rates[ds] = sum(rates) / len(rates)
        else:
            avg_miss_rates[ds] = 0
    
    # Sort data structures by average miss rate (descending)
    sorted_ds = sorted(data_structures, key=lambda x: avg_miss_rates[x], reverse=True)
    
    # Color palette
    colors = {
        'priority_queue': '#8dd3c7',
        'clauses': '#ffffb3',
        'variables': '#bebada',
        'watchlist': '#fb8072'
    }
    
    # Plot stacked area chart for data structure breakdown
    y_data = []
    for ds in sorted_ds:
        y_data.append(miss_rate_data[ds])
    
    ax.stackplot(cache_sizes, *y_data,
                labels=[labels_map[ds] for ds in sorted_ds],
                colors=[colors[ds] for ds in sorted_ds],
                alpha=0.7)
    
    # Plot total miss rate as a line
    ax.plot(cache_sizes, miss_rate_data['total'], 
           color='red', linewidth=2.5, marker='o', markersize=6,
           label='Total', linestyle='-', zorder=10)
    
    # Formatting
    ax.set_xlabel('L1 Cache Size (KB)', fontsize=12, fontweight='bold')
    ax.set_ylabel('Miss Rate (%)', fontsize=12, fontweight='bold')
    ax.set_title('L1 Cache Miss Rate vs. Cache Size', fontsize=14, fontweight='bold')
    ax.grid(True, alpha=0.3, linestyle='--')
    
    # Use log2 scale for x-axis
    ax.set_xscale('log', base=2)
    ax.set_xlim(min(cache_sizes) * 0.9, max(cache_sizes) * 1.1)
    ax.set_ylim(0, max(miss_rate_data['total']) * 1.15 if miss_rate_data['total'] else 100)
    
    # Set x-axis ticks to show cache sizes explicitly
    ax.set_xticks(cache_sizes)
    ax.set_xticklabels([str(s) for s in cache_sizes])
    
    # Legend ranked by average miss rate (total line first, then data structures)
    handles, labels = ax.get_legend_handles_labels()
    # Reorder to put Total first, then others in descending order
    total_idx = labels.index('Total')
    total_handle = handles.pop(total_idx)
    total_label = labels.pop(total_idx)
    
    # Insert total at the beginning
    handles.insert(0, total_handle)
    labels.insert(0, total_label)
    
    ax.legend(handles, labels, loc='upper right', fontsize=10, title='Data Structure')
    
    plt.tight_layout()
    plt.savefig(output_pdf, format='pdf', bbox_inches='tight', dpi=300)
    print(f"\nPlot saved to: {output_pdf}")
    plt.close()


def plot_miss_rate_trends(cache_sizes_dir, base_folder_name, output_pdf=None):
    """Main function to collect data and generate miss rate plot.
    
    Args:
        cache_sizes_dir: Directory containing cache size folders
        base_folder_name: Base folder name to look for under each cache size
        output_pdf: Output PDF file path
    """
    print(f"Collecting miss rate data from: {cache_sizes_dir}")
    print(f"Base folder: {base_folder_name}")
    
    cache_sizes, miss_rate_data = collect_miss_rate_data(cache_sizes_dir, base_folder_name)
    
    if cache_sizes is None or not cache_sizes:
        return
    
    print(f"\n=== Miss Rate Summary ===")
    print(f"Cache sizes: {cache_sizes} KB")
    print(f"\nMiss rates by cache size:")
    for i, size in enumerate(cache_sizes):
        print(f"\n{size} KB:")
        print(f"  Total: {miss_rate_data['total'][i]:.2f}%")
        print(f"  Priority Queue: {miss_rate_data['priority_queue'][i]:.2f}%")
        print(f"  Clauses: {miss_rate_data['clauses'][i]:.2f}%")
        print(f"  Variables: {miss_rate_data['variables'][i]:.2f}%")
        print(f"  Watchlist: {miss_rate_data['watchlist'][i]:.2f}%")
    
    if output_pdf:
        plot_miss_rates(cache_sizes, miss_rate_data, output_pdf)


def main():
    if len(sys.argv) < 3:
        print("Usage: python plot_l1_miss_rate.py <cache_sizes_dir> <base_folder_name> [output.pdf]")
        print("Example: python plot_l1_miss_rate.py ../results base_l1_4_1_l2_8_32 l1_miss_rate.pdf")
        sys.exit(1)
    
    cache_sizes_dir = sys.argv[1]
    base_folder_name = sys.argv[2]
    output_pdf = sys.argv[3] if len(sys.argv) > 3 else None
    
    if output_pdf is None:
        # Auto-generate output filename
        output_pdf = f"l1_miss_rate_{base_folder_name}.pdf"
    
    plot_miss_rate_trends(cache_sizes_dir, base_folder_name, output_pdf)


if __name__ == "__main__":
    main()
