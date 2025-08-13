#!/usr/bin/env python3
"""
Unified SAT Solver Log Parser

This module provides a unified parser for SAT solver log files that extracts:
- Test case name and SAT/UNSAT result
- Number of variables and clauses  
- Total memory usage across all components
- Solver statistics (decisions, conflicts, propagations, etc.)
- L1 Cache Profiler Statistics by component
- Clauses Fragmentation statistics
- Cycle Statistics
- Simulated time

Usage:
    from unified_parser import parse_log_file, parse_log_directory
    
    # Parse single file
    data = parse_log_file('path/to/logfile.log')
    
    # Parse all files in directory
    all_data = parse_log_directory('path/to/logs/')
"""

import os
import re
from pathlib import Path


def format_bytes(bytes_value):
    """Convert bytes to appropriate unit (KB/MB/GB) with rounding."""
    if bytes_value < 1024:
        return f"{bytes_value} B"
    elif bytes_value < 1024 * 1024:
        return f"{bytes_value / 1024:.1f} KB"
    elif bytes_value < 1024 * 1024 * 1024:
        return f"{bytes_value / (1024 * 1024):.1f} MB"
    else:
        return f"{bytes_value / (1024 * 1024 * 1024):.1f} GB"


def parse_solver_statistics(content):
    """Parse solver statistics section."""
    stats = {}
    
    # Find solver statistics section
    solver_section = re.search(
        r'============================\[ Solver Statistics \]============================\n(.*?)\n=+',
        content, re.DOTALL
    )
    
    if solver_section:
        stats_text = solver_section.group(1)
        
        # Parse each statistic
        patterns = {
            'decisions': r'Decisions\s*:\s*(\d+)',
            'propagations': r'Propagations\s*:\s*(\d+)',
            'conflicts': r'Conflicts\s*:\s*(\d+)',
            'learned': r'Learned\s*:\s*(\d+)',
            'removed': r'Removed\s*:\s*(\d+)',
            'db_reductions': r'DB_Reductions\s*:\s*(\d+)',
            'assigns': r'Assigns\s*:\s*(\d+)',
            'unassigns': r'UnAssigns\s*:\s*(\d+)',
            'minimized': r'Minimized\s*:\s*(\d+)',
            'restarts': r'Restarts\s*:\s*(\d+)'
        }
        
        for key, pattern in patterns.items():
            match = re.search(pattern, stats_text)
            if match:
                stats[key] = int(match.group(1))
            else:
                stats[key] = 0
    
    return stats


def parse_l1_cache_statistics(content):
    """Parse L1 Cache Profiler Statistics section."""
    cache_stats = {}
    
    # Find L1 cache section
    section_pattern = r'===+\s*L1 Cache Profiler Statistics\s*===+\n(.*?)\n===+'
    section_match = re.search(section_pattern, content, re.DOTALL)
    
    if section_match:
        section_text = section_match.group(1)
        
        # Parse total statistics first
        total_pattern = r'TOTAL\s*:\s*(\d+) hits,\s*(\d+) misses,\s*(\d+) total,\s*([\d.]+)% miss rate'
        total_match = re.search(total_pattern, section_text)
        if total_match:
            cache_stats['l1_total_requests'] = int(total_match.group(3))
            cache_stats['l1_total_miss_rate'] = float(total_match.group(4))
        
        # Parse component statistics (excluding ClaActivity)
        components = ['Heap', 'Variables', 'Watches', 'Clauses', 'VarActivity']
        for component in components:
            pattern = f'{component}\\s*:\\s*(\\d+) hits,\\s*(\\d+) misses,\\s*(\\d+) total,\\s*([\\d.]+)% miss rate'
            match = re.search(pattern, section_text)
            if match:
                comp_name = component.lower()
                cache_stats[f'l1_{comp_name}_total'] = int(match.group(3))
                cache_stats[f'l1_{comp_name}_miss_rate'] = float(match.group(4))
                cache_stats[f'l1_{comp_name}_hits'] = int(match.group(1))
                cache_stats[f'l1_{comp_name}_misses'] = int(match.group(2))
    
    return cache_stats


def parse_clauses_fragmentation(content):
    """Parse Clauses Fragmentation section."""
    frag_stats = {}
    
    # Find fragmentation section
    frag_section = re.search(
        r'=+\[ Clauses Fragmentation \]=+\n(.*?)\n=+',
        content, re.DOTALL
    )
    
    if frag_section:
        frag_text = frag_section.group(1)
        
        patterns = {
            'heap_bytes': r'Heap:\s*(\d+)\s*bytes',
            'reserved_bytes': r'Reserved:\s*(\d+)\s*bytes',
            'requested_bytes': r'Requested:\s*(\d+)\s*bytes',
            'allocated_bytes': r'Allocated:\s*(\d+)\s*bytes',
            'wasted_bytes': r'Wasted:\s*(\d+)\s*bytes',
            'current_frag_percent': r'Current frag:\s*([\d.]+)%',
            'peak_frag_percent': r'Peak frag:\s*([\d.]+)%'
        }
        
        for key, pattern in patterns.items():
            match = re.search(pattern, frag_text)
            if match:
                if 'percent' in key:
                    frag_stats[key] = float(match.group(1))
                else:
                    frag_stats[key] = int(match.group(1))
    
    return frag_stats


def parse_cycle_statistics(content):
    """Parse Cycle Statistics section."""
    cycle_stats = {}
    
    # Find cycle statistics section
    cycle_section = re.search(
        r'===+\[ Cycle Statistics \]===+\n(.*?)\n=+',
        content, re.DOTALL
    )
    
    if cycle_section:
        cycle_text = cycle_section.group(1)
        
        # Parse individual cycle types
        cycle_patterns = {
            'propagate_cycles': r'Propagate\s*:\s*[\d.]+%\s*\((\d+) cycles\)',
            'analyze_cycles': r'Analyze\s*:\s*[\d.]+%\s*\((\d+) cycles\)',
            'minimize_cycles': r'Minimize\s*:\s*[\d.]+%\s*\((\d+) cycles\)',
            'backtrack_cycles': r'Backtrack\s*:\s*[\d.]+%\s*\((\d+) cycles\)',
            'decision_cycles': r'Decision\s*:\s*[\d.]+%\s*\((\d+) cycles\)',
            'reduce_db_cycles': r'Reduce DB\s*:\s*[\d.]+%\s*\((\d+) cycles\)',
            'restart_cycles': r'Restart\s*:\s*[\d.]+%\s*\((\d+) cycles\)',
            'total_counted_cycles': r'Total Counted:\s*(\d+) cycles'
        }
        
        for key, pattern in cycle_patterns.items():
            match = re.search(pattern, cycle_text)
            if match:
                cycle_stats[key] = int(match.group(1))
    
    return cycle_stats


def parse_parallel_histograms(content):
    """Parse Parallel Watchers and Variables Histogram sections."""
    histogram_stats = {}
    
    # Parse Watchers Histogram
    watchers_section = re.search(
        r'=+\[ Parallel Watchers Histogram \]=+\n(.*?)\n=+',
        content, re.DOTALL
    )
    
    if watchers_section:
        watchers_text = watchers_section.group(1)
        
        # Get total samples
        total_match = re.search(r'Total samples: (\d+)', watchers_text)
        if total_match:
            histogram_stats['watchers_total_samples'] = int(total_match.group(1))
        
        # Parse each bin
        bins = {}
        bin_pattern = r'Bin \[\s*(\d+)-\s*(\d+)\]:\s+(\d+) samples \(([\d.]+)%\)'
        for match in re.finditer(bin_pattern, watchers_text):
            bin_start = int(match.group(1))
            bin_end = int(match.group(2))
            samples = int(match.group(3))
            percentage = float(match.group(4))
            
            # Use bin start as key for single-value bins
            if bin_start == bin_end:
                bin_key = bin_start
            else:
                bin_key = f"{bin_start}-{bin_end}"
                
            bins[bin_key] = {'samples': samples, 'percentage': percentage}
        
        # Parse out of bounds
        out_of_bounds_match = re.search(r'Out of bounds:\s+(\d+) samples \(([\d.]+)%\)', watchers_text)
        if out_of_bounds_match:
            bins['out_of_bounds'] = {
                'samples': int(out_of_bounds_match.group(1)),
                'percentage': float(out_of_bounds_match.group(2))
            }
            
        histogram_stats['watchers_bins'] = bins
    
    # Parse Variables Histogram
    variables_section = re.search(
        r'=+\[ Parallel Variables Histogram \]=+\n(.*?)\n=+',
        content, re.DOTALL
    )
    
    if variables_section:
        variables_text = variables_section.group(1)
        
        # Get total samples
        total_match = re.search(r'Total samples: (\d+)', variables_text)
        if total_match:
            histogram_stats['variables_total_samples'] = int(total_match.group(1))
        
        # Parse each bin
        bins = {}
        bin_pattern = r'Bin \[\s*(\d+)-\s*(\d+)\]:\s+(\d+) samples \(([\d.]+)%\)'
        for match in re.finditer(bin_pattern, variables_text):
            bin_start = int(match.group(1))
            bin_end = int(match.group(2))
            samples = int(match.group(3))
            percentage = float(match.group(4))
            
            # Use bin start as key for single-value bins
            if bin_start == bin_end:
                bin_key = bin_start
            else:
                bin_key = f"{bin_start}-{bin_end}"
                
            bins[bin_key] = {'samples': samples, 'percentage': percentage}
        
        # Parse out of bounds
        out_of_bounds_match = re.search(r'Out of bounds:\s+(\d+) samples \(([\d.]+)%\)', variables_text)
        if out_of_bounds_match:
            bins['out_of_bounds'] = {
                'samples': int(out_of_bounds_match.group(1)),
                'percentage': float(out_of_bounds_match.group(2))
            }
            
        histogram_stats['variables_bins'] = bins
    
    return histogram_stats


def parse_log_file(log_file_path):
    """
    Parse a single log file and extract all relevant information.
    
    Returns a dictionary containing:
    - Basic info: test_case, result, variables, clauses
    - Memory: total_memory_bytes, total_memory_formatted  
    - Solver stats: decisions, propagations, conflicts, etc.
    - L1 cache stats: l1_total_requests, l1_total_miss_rate, l1_{component}_total, l1_{component}_miss_rate
    - Fragmentation: heap_bytes, reserved_bytes, etc.
    - Cycles: propagate_cycles, analyze_cycles, etc.
    - Timing: sim_time_ms
    - Histogram data: watchers and variables histograms
    """
    result = {
        'test_case': '',
        'result': '',
        'variables': 0,
        'clauses': 0,
        'total_memory_bytes': 0,
        'total_memory_formatted': '',
        'sim_time_ms': 0.0
    }
    
    try:
        with open(log_file_path, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
        
        # Extract test case name from filename
        filename = os.path.basename(log_file_path)
        # Remove the timestamp and .log extension
        test_case_match = re.match(r'(.+?)_(sat|unsat)_\d{8}_\d{6}\.log$', filename)
        if test_case_match:
            result['test_case'] = test_case_match.group(1)
            result['result'] = test_case_match.group(2).upper()
        else:
            # Fallback: use filename without extension
            result['test_case'] = os.path.splitext(filename)[0]
        
        # Extract variables and clauses from "MAIN-> Problem:" line
        problem_match = re.search(r'MAIN-> Problem: vars=(\d+) clauses=(\d+)', content)
        if problem_match:
            result['variables'] = int(problem_match.group(1))
            result['clauses'] = int(problem_match.group(2))
        
        # Extract memory usage from size lines
        memory_patterns = [
            (r'VAR-> Size: \d+ variables, (\d+) bytes', 'variables'),
            (r'WATCH-> Size: \d+ watches, (\d+) bytes', 'watches'),
            (r'WATCH-> Size: \d+ watch node blocks, (\d+) bytes', 'watch_nodes'),
            (r'CLAUSES-> Size: \d+ clause pointers, (\d+) bytes', 'clause_pointers'),
            (r'CLAUSES-> Size: \d+ clause structs, (\d+) bytes', 'clause_structs'),
            (r'HEAP-> Size: \d+ decision variables, (\d+) bytes', 'heap_decisions'),
            (r'HEAP-> Size: \d+ indices, (\d+) bytes', 'heap_indices'),
            (r'VAR_ACT-> Size: \d+ var activities, (\d+) bytes', 'var_activities')
        ]
        
        total_bytes = 0
        for pattern, component in memory_patterns:
            match = re.search(pattern, content)
            if match:
                bytes_value = int(match.group(1))
                total_bytes += bytes_value
        
        result['total_memory_bytes'] = total_bytes
        result['total_memory_formatted'] = format_bytes(total_bytes)
        
        # Double-check result from the actual solver output
        if 'UNSATISFIABLE' in content:
            result['result'] = 'UNSAT'
        elif 'SATISFIABLE' in content and 'UNSATISFIABLE' not in content:
            result['result'] = 'SAT'
        
        # Extract simulated time
        time_match = re.search(r'Simulation is complete, simulated time: ([\d.]+)\s*(\w+)', content)
        if time_match:
            time_val = float(time_match.group(1))
            time_unit = time_match.group(2)
            # Convert to milliseconds
            if time_unit == 'us':
                time_val *= 0.001
            elif time_unit == 's':
                time_val *= 1000
            # Default assumption is ms
            result['sim_time_ms'] = time_val
        
        # Parse all sections
        solver_stats = parse_solver_statistics(content)
        result.update(solver_stats)
        
        cache_stats = parse_l1_cache_statistics(content)
        result.update(cache_stats)
        
        frag_stats = parse_clauses_fragmentation(content)
        result.update(frag_stats)
        
        cycle_stats = parse_cycle_statistics(content)
        result.update(cycle_stats)
        
        # Add histogram parsing
        histogram_stats = parse_parallel_histograms(content)
        result.update(histogram_stats)
        
    except Exception as e:
        print(f"Error parsing {log_file_path}: {e}")
        return None
    
    return result


def parse_log_directory(logs_dir, exclude_summary=True):
    """
    Parse all log files in a directory.
    
    Args:
        logs_dir: Path to directory containing log files
        exclude_summary: If True, skip files with 'summary' in the name
    
    Returns:
        List of dictionaries, one per successfully parsed log file
    """
    logs_dir = Path(logs_dir)
    
    if not logs_dir.exists():
        print(f"Error: Directory {logs_dir} does not exist")
        return []
    
    log_files = list(logs_dir.glob("*.log"))
    if not log_files:
        print(f"No .log files found in {logs_dir}")
        return []
    
    results = []
    failed_files = []
    
    for log_file in sorted(log_files):
        # Skip summary files if requested
        if exclude_summary and 'summary' in log_file.name.lower():
            continue
            
        result = parse_log_file(log_file)
        if result:
            results.append(result)
        else:
            failed_files.append(log_file.name)
    
    if failed_files:
        print(f"Failed to parse {len(failed_files)} files: {failed_files}")
    
    return results


def get_cache_size_from_directory(directory_name):
    """Extract cache size in bytes from directory name like 'logs_4MiB'."""
    size_match = re.search(r'logs_(?:ddr_)?(\d+)([KMG]i?B)', directory_name)
    if not size_match:
        return None
    
    size_num = int(size_match.group(1))
    size_unit = size_match.group(2)
    
    # Convert to bytes
    multipliers = {
        'KiB': 1024, 'KB': 1000,
        'MiB': 1024*1024, 'MB': 1000*1000,
        'GiB': 1024*1024*1024, 'GB': 1000*1000*1000
    }
    
    return size_num * multipliers.get(size_unit, 1)
