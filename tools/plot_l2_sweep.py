#!/usr/bin/env python3
"""
L2 Latency and Bandwidth Sweep Plotter

This script plots PAR-2 scores over L2 latency and bandwidth in two subplots.

The script uses hardcoded lists of folder names for bandwidth and latency sweeps:
- Latency sweep: base_l1_4_1_l2_8_* folders with varying latencies (32, 64, 96, 128, 160 cycles)
- Bandwidth sweep: base_l1_-1_1_l2_*_32_*B folders with varying bandwidths
  For bandwidth folders, actual BW (GB/s) = width (in bytes) * 2

Each folder may contain multiple seed folders or a single seed.
PAR-2 scoring is used: solved tests use actual time, unsolved/timeout tests
are penalized with 2*timeout (default timeout: 36s).

Usage: python plot_l2_sweep.py <base_directory> [output.pdf] [--timeout SECONDS]
Example: python plot_l2_sweep.py ../sat-isca26-data l2_sweep.pdf
Example: python plot_l2_sweep.py ../sat-isca26-data l2_sweep.pdf --timeout 36
"""

import sys
from pathlib import Path
import matplotlib.pyplot as plt
import numpy as np
from unified_parser import parse_log_directory
import re


def parse_l2_config(folder_name):
    """Extract L2 bandwidth and latency from folder name.
    
    Args:
        folder_name: e.g., "base_l1_4_1_l2_8_32" or "base_l1_-1_1_l2_1_32_64B"
    
    Returns:
        (bandwidth_gbps, latency) tuple or (None, None) if parse fails
        For folders with width suffix (e.g., _64B), bandwidth = width * 2 (GB/s)
        For folders without width suffix, bandwidth = multiplier * 8 (GB/s)
    """
    # Try pattern with width suffix: base_l1_-1_1_l2_<mult>_<lat>_<width>B
    match_with_width = re.search(r'_l2_(\d+)_(\d+)_(\d+)B', folder_name)
    if match_with_width:
        # multiplier = int(match_with_width.group(1))  # Not used in this case
        latency = int(match_with_width.group(2))
        width_bytes = int(match_with_width.group(3))
        bandwidth_gbps = width_bytes * 2  # width * 2 GB/s
        return bandwidth_gbps, latency
    
    # Try pattern without width suffix: base_l1_4_1_l2_<mult>_<lat>
    match_no_width = re.search(r'_l2_(\d+)_(\d+)(?:_|$)', folder_name)
    if match_no_width:
        multiplier = int(match_no_width.group(1))
        latency = int(match_no_width.group(2))
        bandwidth_gbps = multiplier * 8  # multiplier * 8 GB/s
        return bandwidth_gbps, latency
    
    return None, None


def compute_par2_score(results, timeout_seconds=3600):
    """Compute PAR-2 score across all tests.
    
    PAR-2 assigns 2*timeout penalty to unsolved/timeout tests.
    
    Args:
        results: List of parsed results
        timeout_seconds: Timeout in seconds (default: 3600)
    
    Returns:
        PAR-2 score in seconds, or None if no results
    """
    if not results:
        return None
    
    timeout_ms = timeout_seconds * 1000.0
    par2_penalty_ms = 2 * timeout_ms
    
    par2_total_ms = 0.0
    
    for r in results:
        sim_time_ms = r.get('sim_time_ms')
        if sim_time_ms is None:
            sim_time_ms = 0.0
        
        result = r.get('result')
        
        # Check if solved within timeout
        if result in ('SAT', 'UNSAT') and sim_time_ms > 0 and sim_time_ms <= timeout_ms:
            par2_total_ms += sim_time_ms
        else:
            # Unknown or exceeded timeout: use 2*timeout penalty
            par2_total_ms += par2_penalty_ms
    
    # Return average PAR-2 in seconds
    return (par2_total_ms / len(results)) / 1000.0


def collect_sweep_data(base_directory, folder_names, timeout_seconds=36):
    """Collect PAR-2 data for specified L2 configurations.
    
    Uses shared seed logic: determines common seeds across all folders and 
    only uses those consistently. For example, if one folder has only seed0, 
    then only seed0 is used for all folders. Computes PAR-2 per seed first, 
    then averages across seeds (matching parse_results.py behavior).
    
    Args:
        base_directory: Base directory containing the folders
        folder_names: List of folder names to process
        timeout_seconds: Timeout in seconds for PAR-2 calculation (default: 36)
    
    Returns:
        dict mapping (bandwidth_gbps, latency) to PAR-2 score
    """
    base_directory = Path(base_directory)
    
    if not base_directory.exists():
        print(f"Error: Directory {base_directory} does not exist")
        return None
    
    # Process specified folders
    matching_folders = []
    for folder_name in folder_names:
        folder = base_directory / folder_name
        if not folder.exists() or not folder.is_dir():
            print(f"Warning: Folder {folder} does not exist, skipping")
            continue
        
        bandwidth_gbps, latency = parse_l2_config(folder_name)
        if bandwidth_gbps is not None and latency is not None:
            matching_folders.append((folder, bandwidth_gbps, latency))
        else:
            print(f"Warning: Could not parse config from {folder_name}, skipping")
    
    if not matching_folders:
        print(f"Error: No valid folders found")
        return None
    
    print(f"Processing {len(matching_folders)} folders")
    print(f"Using timeout: {timeout_seconds}s for PAR-2 calculation")
    
    # Determine shared seeds across all folders
    all_seed_sets = []
    for folder, _, _ in matching_folders:
        seed_dirs = sorted([d.name for d in folder.glob('seed*') if d.is_dir()])
        if seed_dirs:
            all_seed_sets.append(set(seed_dirs))
        else:
            # No seed structure, treat as single-seed at folder level
            all_seed_sets.append(set())
    
    # Find intersection of all seed sets to get shared seeds
    shared_seeds = None
    if all_seed_sets and all(len(s) > 0 for s in all_seed_sets):
        shared_seeds = sorted(set.intersection(*all_seed_sets))
        if shared_seeds:
            print(f"Using shared seeds: {shared_seeds}")
        else:
            print("No shared seeds found across folders")
            shared_seeds = None
    else:
        print("Not all folders have seed structure")
    
    # Collect data
    sweep_data = {}
    
    for folder, bandwidth, latency in matching_folders:
        print(f"\nProcessing {folder.name} (BW={bandwidth}, Lat={latency})")
        
        # Check if this folder has seed subdirectories
        seed_dirs = sorted([d for d in folder.glob('seed*') if d.is_dir()])
        
        if shared_seeds and seed_dirs:
            # Use only the shared seeds that exist across all folders
            per_seed_par2 = []
            
            for seed_name in shared_seeds:
                seed_dir = folder / seed_name
                if not seed_dir.exists():
                    print(f"  Warning: {seed_name} not found in {folder.name}")
                    continue
                
                results = parse_log_directory(seed_dir, exclude_summary=True)
                if not results:
                    print(f"  Warning: No results in {seed_name}")
                    continue
                
                par2 = compute_par2_score(results, timeout_seconds)
                if par2 is not None:
                    per_seed_par2.append(par2)
            
            if per_seed_par2:
                # Average PAR-2 across seeds
                avg_par2 = sum(per_seed_par2) / len(per_seed_par2)
                sweep_data[(bandwidth, latency)] = avg_par2
                
                # Count finished tests across shared seeds
                total_tests = 0
                finished_tests = 0
                for seed_name in shared_seeds:
                    seed_dir = folder / seed_name
                    if seed_dir.exists():
                        results = parse_log_directory(seed_dir, exclude_summary=True)
                        if results:
                            total_tests += len(results)
                            timeout_ms = timeout_seconds * 1000.0
                            finished_tests += sum(1 for r in results 
                                                if r.get('result') in ('SAT', 'UNSAT') 
                                                and (r.get('sim_time_ms') or 0) <= timeout_ms)
                
                print(f"  Seeds: {len(per_seed_par2)}, Total tests: {total_tests}, Finished: {finished_tests}")
                print(f"  PAR-2 scores per seed: {[f'{p:.2f}' for p in per_seed_par2]}")
                print(f"  Average PAR-2: {avg_par2:.2f} seconds")
            else:
                print(f"  No valid PAR-2 scores computed")
        else:
            # Single seed case: parse the folder directly (no seed structure)
            all_results = parse_log_directory(folder, exclude_summary=True)
            
            if not all_results:
                print(f"  No valid results found")
                continue
            
            timeout_ms = timeout_seconds * 1000.0
            finished = [r for r in all_results 
                       if r.get('result') in ('SAT', 'UNSAT') 
                       and (r.get('sim_time_ms') or 0) <= timeout_ms]
            print(f"  Total tests: {len(all_results)}, Finished: {len(finished)}")
            
            par2_score = compute_par2_score(all_results, timeout_seconds)
            if par2_score is not None:
                sweep_data[(bandwidth, latency)] = par2_score
                print(f"  PAR-2 score: {par2_score:.2f} seconds")
    
    if not sweep_data:
        print("\nError: No valid data collected")
        return None
    
    return sweep_data


def organize_data_for_plotting(bw_sweep_data, lat_sweep_data):
    """Organize sweep data into separate datasets for bandwidth and latency sweeps.
    
    Args:
        bw_sweep_data: Data points for bandwidth sweep (may be None)
        lat_sweep_data: Data points for latency sweep (may be None)
    
    Returns:
        (bandwidth_data, latency_data) where each is a dict:
        {fixed_param: [(swept_param, par2_score), ...]}
        
    A sweep is considered valid only if there are at least 2 points with the fixed parameter.
    """
    bandwidth_data = {}
    latency_data = {}
    
    # Process bandwidth sweep data
    if bw_sweep_data:
        # Group by fixed latency (for bandwidth sweep)
        bandwidth_data_raw = {}
        for (bw, lat), runtime in bw_sweep_data.items():
            if lat not in bandwidth_data_raw:
                bandwidth_data_raw[lat] = []
            bandwidth_data_raw[lat].append((bw, runtime))
        
        # Filter to only include latencies with at least 2 bandwidth points (valid sweep)
        for lat, points in bandwidth_data_raw.items():
            if len(points) >= 2:
                points.sort(key=lambda x: x[0])
                bandwidth_data[lat] = points
    
    # Process latency sweep data
    if lat_sweep_data:
        # Group by fixed bandwidth (for latency sweep)
        latency_data_raw = {}
        for (bw, lat), runtime in lat_sweep_data.items():
            if bw not in latency_data_raw:
                latency_data_raw[bw] = []
            latency_data_raw[bw].append((lat, runtime))
        
        # Filter to only include bandwidths with at least 2 latency points (valid sweep)
        for bw, points in latency_data_raw.items():
            if len(points) >= 2:
                points.sort(key=lambda x: x[0])
                latency_data[bw] = points
    
    return bandwidth_data, latency_data


def plot_sweeps(bw_sweep_data, lat_sweep_data, output_pdf):
    """Create a PDF plot with two subplots: PAR-2 vs bandwidth (top) and PAR-2 vs latency (bottom).
    
    If only one type of sweep is found, still create both subplots but leave the empty one blank.
    Assumes only one sweep per subplot (no legends shown).
    Both subplots use the same y-axis scale for consistency.
    
    Args:
        bw_sweep_data: Data points for bandwidth sweep
        lat_sweep_data: Data points for latency sweep
        output_pdf: Output PDF file path
    """
    bandwidth_data, latency_data = organize_data_for_plotting(bw_sweep_data, lat_sweep_data)
    
    has_bandwidth_sweep = len(bandwidth_data) > 0
    has_latency_sweep = len(latency_data) > 0
    
    if not has_bandwidth_sweep and not has_latency_sweep:
        print("Warning: No valid sweeps found (need at least 2 points for a sweep)")
        return
    
    # Create side-by-side subplots (1 row, 2 columns) with larger figure size
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))
    
    # Collect all PAR-2 values to determine common y-axis limits
    all_par2_values = []
    if has_bandwidth_sweep:
        for lat, points in bandwidth_data.items():
            all_par2_values.extend([par2 for _, par2 in points])
    if has_latency_sweep:
        for bw, points in latency_data.items():
            all_par2_values.extend([par2 for _, par2 in points])
    
    # Determine y-axis limits with some padding
    if all_par2_values:
        y_min = min(all_par2_values)
        y_max = max(all_par2_values)
        y_range = y_max - y_min
        y_padding = y_range * 0.1 if y_range > 0 else 1.0
        y_limits = (y_min - y_padding, y_max + y_padding)
    else:
        y_limits = None
    
    # Plot 1: PAR-2 vs L2 Bandwidth (fixed latencies)
    if has_bandwidth_sweep:
        # Assume only one latency (no legend needed)
        sorted_latencies = sorted(bandwidth_data.keys())
        lat = sorted_latencies[0]  # Take first (and assumed only) latency
        bw_gbps, par2_values = zip(*bandwidth_data[lat])
        # Bandwidth is already in GB/s from parse_l2_config
        ax1.plot(bw_gbps, par2_values, marker='o', markersize=12, linewidth=3.5, color='#1f77b4')
    else:
        ax1.text(0.5, 0.5, 'Bandwidth sweep data not available', 
                ha='center', va='center', transform=ax1.transAxes, fontsize=20)
    
    ax1.set_xlabel('L2 Bandwidth (GB/s)', fontsize=32, fontweight='bold')
    ax1.set_ylabel('PAR-2 (s)', fontsize=32, fontweight='bold')
    ax1.grid(True, alpha=0.3, linestyle='--')
    ax1.tick_params(axis='both', which='major', labelsize=28)
    if y_limits:
        ax1.set_ylim(y_limits)
    
    # Plot 2: PAR-2 vs L2 Latency (fixed bandwidths)
    if has_latency_sweep:
        # Assume only one bandwidth (no legend needed)
        sorted_bandwidths = sorted(latency_data.keys())
        bw = sorted_bandwidths[0]  # Take first (and assumed only) bandwidth
        lat_values, par2_values = zip(*latency_data[bw])
        ax2.plot(lat_values, par2_values, marker='o', markersize=12, linewidth=3.5, color='#ff7f0e')
    else:
        ax2.text(0.5, 0.5, 'Latency sweep data not available', 
                ha='center', va='center', transform=ax2.transAxes, fontsize=20)
    
    ax2.set_xlabel('L2 Latency (cycles)', fontsize=32, fontweight='bold')
    ax2.set_ylabel('PAR-2 (s)', fontsize=32, fontweight='bold')
    ax2.grid(True, alpha=0.3, linestyle='--')
    ax2.tick_params(axis='both', which='major', labelsize=28)
    if y_limits:
        ax2.set_ylim(y_limits)
    
    plt.tight_layout()
    plt.savefig(output_pdf, format='pdf', bbox_inches='tight', dpi=300)
    print(f"\nPlot saved to: {output_pdf}")
    plt.close()


def plot_l2_sweep(base_directory, output_pdf=None, timeout_seconds=36):
    """Main function to collect data and generate L2 sweep plots.
    
    Args:
        base_directory: Base directory containing the sweep folders
        output_pdf: Output PDF file path
        timeout_seconds: Timeout in seconds for PAR-2 calculation (default: 36)
    """
    # Hardcoded folder names for latency sweep
    latency_folders = [
        'base_128KB/base_l1_4_1_l2_8_32',
        'base_128KB/base_l1_4_1_l2_8_64',
        'base_128KB/base_l1_4_1_l2_8_96',
        'base_128KB/base_l1_4_1_l2_8_128',
        'base_128KB/base_l1_4_1_l2_8_160',
    ]
    
    # Hardcoded folder names for bandwidth sweep
    bandwidth_folders = [
        # 'bw_simpmem/base_l1_-1_1_l2_1_32_32B',
        # 'bw_simpmem/base_l1_-1_1_l2_1_32_64B',
        # 'bw_simpmem/base_l1_-1_1_l2_2_32_64B',
        # 'bw_simpmem/base_l1_-1_1_l2_4_32_64B',
        'bw/base_l1_-1_1_l2_1_32_8B',
        'bw/base_l1_-1_1_l2_1_32_16B',
        'bw/base_l1_-1_1_l2_1_32_32B',
        'bw/base_l1_-1_1_l2_1_32_64B',
        'bw/base_l1_-1_1_l2_2_32_128B',
    ]
    
    print(f"Base directory: {base_directory}")
    
    # Collect data for bandwidth sweep
    print("\n=== Collecting Bandwidth Sweep Data ===")
    bw_sweep_data = collect_sweep_data(base_directory, bandwidth_folders, timeout_seconds)
    
    # Collect data for latency sweep
    print("\n=== Collecting Latency Sweep Data ===")
    lat_sweep_data = collect_sweep_data(base_directory, latency_folders, timeout_seconds)
    
    if (bw_sweep_data is None or not bw_sweep_data) and (lat_sweep_data is None or not lat_sweep_data):
        print("Error: No valid data collected")
        return
    
    print(f"\n=== Sweep Data Summary ===")
    if bw_sweep_data:
        print(f"Bandwidth sweep configurations: {len(bw_sweep_data)}")
    if lat_sweep_data:
        print(f"Latency sweep configurations: {len(lat_sweep_data)}")
    
    # Organize and print summary
    bandwidth_data, latency_data = organize_data_for_plotting(bw_sweep_data, lat_sweep_data)
    
    if bandwidth_data:
        print(f"\nBandwidth sweep (fixed latencies): {len(bandwidth_data)} latency points")
        for lat in sorted(bandwidth_data.keys()):
            print(f"  Latency = {lat} cycles: {len(bandwidth_data[lat])} bandwidth points")
    else:
        print(f"\nBandwidth sweep: No valid sweep found (need at least 2 bandwidth points with same latency)")
    
    if latency_data:
        print(f"\nLatency sweep (fixed bandwidths): {len(latency_data)} bandwidth points")
        for bw in sorted(latency_data.keys()):
            print(f"  Bandwidth = {bw} GB/s: {len(latency_data[bw])} latency points")
    else:
        print(f"\nLatency sweep: No valid sweep found (need at least 2 latency points with same bandwidth)")
    
    if output_pdf and (bandwidth_data or latency_data):
        plot_sweeps(bw_sweep_data, lat_sweep_data, output_pdf)
    elif not (bandwidth_data or latency_data):
        print("\nError: No valid sweeps found. Need at least 2 points for a sweep.")


def main():
    if len(sys.argv) < 2:
        print("Usage: python plot_l2_sweep.py <base_directory> [output.pdf] [--timeout SECONDS]")
        print("\nArguments:")
        print("  base_directory           Base directory containing sweep folders")
        print("  output.pdf               Output PDF file (optional, default: l2_sweep.pdf)")
        print("\nOptions:")
        print("  --timeout SECONDS        Timeout for PAR-2 calculation (default: 36)")
        print("\nExample:")
        print("  python plot_l2_sweep.py ../sat-isca26-data l2_sweep.pdf")
        print("  python plot_l2_sweep.py ../sat-isca26-data l2_sweep.pdf --timeout 36")
        sys.exit(1)
    
    base_directory = sys.argv[1]
    output_pdf = None
    timeout_seconds = 36  # Default timeout
    
    # Parse remaining arguments
    i = 2
    while i < len(sys.argv):
        if sys.argv[i] == '--timeout' and i + 1 < len(sys.argv):
            timeout_seconds = float(sys.argv[i + 1])
            i += 2
        else:
            # Assume it's the output file if not a flag
            if output_pdf is None and not sys.argv[i].startswith('--'):
                output_pdf = sys.argv[i]
            i += 1
    
    if output_pdf is None:
        # Auto-generate output filename
        output_pdf = "l2_sweep.pdf"
    
    plot_l2_sweep(base_directory, output_pdf, timeout_seconds)


if __name__ == "__main__":
    main()
