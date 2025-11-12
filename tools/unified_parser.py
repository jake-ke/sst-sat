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
import csv
from pathlib import Path


def detect_log_format(content):
    """Detect whether this is a minisat or satsolver log."""
    # Check for satsolver format indicators
    if 'Using CNF file:' in content and 'Simulation is complete' in content:
        return 'satsolver'
    # Check for minisat format indicators
    elif 'Problem Statistics' in content and 'conflicts             :' in content:
        return 'minisat'
    else:
        return 'unknown'


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


def parse_minisat_log(log_file_path, content):
    """Parse minisat format log file.
    
    Extracts a subset of information available in minisat logs:
    - Test case name and SAT/UNSAT result (or TIMEDOUT/UNKNOWN)
    - Number of variables and clauses
    - Memory used
    - Runtime (converted to ms for consistency)
    - Solver statistics (decisions, propagations, conflicts, etc.)
    
    Always returns a result dictionary with whatever data could be parsed.
    """
    result = {
        'test_case': '',
        'result': 'UNKNOWN',
        'variables': 0,
        'clauses': 0,
        'total_memory_bytes': 0,
        'total_memory_formatted': '0 B',
        'sim_time_ms': 0.0,
        'decisions': 0,
        'propagations': 0,
        'conflicts': 0,
        'learned': 0,
        'removed': 0,
        'db_reductions': 0,
        'minimized': 0,
        'restarts': 0,
    }
    
    try:
        # Extract test case name from filename
        filename = os.path.basename(log_file_path)
        # Remove .log extension first
        base_no_log = re.sub(r'\.log$', '', filename)
        # Remove timestamp pattern _YYYYMMDD_HHMMSS if present
        test_case = re.sub(r'_\d{8}_\d{6}$', '', base_no_log)
        # Remove compression extensions
        test_case = re.sub(r'\.(xz|gz|bz2)$', '', test_case)
        result['test_case'] = test_case
        
        # Extract variables and clauses from Problem Statistics section
        vars_match = re.search(r'Number of variables:\s*(\d+)', content)
        if vars_match:
            result['variables'] = int(vars_match.group(1))
        
        clauses_match = re.search(r'Number of clauses:\s*(\d+)', content)
        if clauses_match:
            result['clauses'] = int(clauses_match.group(1))
        
        # Extract memory usage (convert to bytes)
        memory_match = re.search(r'Memory used\s*:\s*([\d.]+)\s*MB', content)
        if memory_match:
            memory_mb = float(memory_match.group(1))
            result['total_memory_bytes'] = int(memory_mb * 1024 * 1024)
            result['total_memory_formatted'] = format_bytes(result['total_memory_bytes'])
        
        # Extract CPU time and convert to milliseconds
        cpu_match = re.search(r'CPU time\s*:\s*([\d.]+)\s*s', content, re.IGNORECASE)
        if cpu_match:
            cpu_seconds = float(cpu_match.group(1))
            result['sim_time_ms'] = cpu_seconds * 1000.0
        else:
            # Fallback: try without 's' unit
            cpu_match_alt = re.search(r'CPU time\s*:\s*([\d.]+)', content, re.IGNORECASE)
            if cpu_match_alt:
                cpu_seconds = float(cpu_match_alt.group(1))
                result['sim_time_ms'] = cpu_seconds * 1000.0
        
        # Extract solver statistics
        solver_patterns = {
            'decisions': r'decisions\s*:\s*(\d+)',
            'propagations': r'propagations\s*:\s*(\d+)',
            'conflicts': r'conflicts\s*:\s*(\d+)',
            'learned': r'learned\s*:\s*(\d+)',
            'removed': r'removed\s*:\s*(\d+)',
            'db_reductions': r'db_reductions\s*:\s*(\d+)',
            'minimized': r'minimized\s*:\s*(\d+)',
            'restarts': r'restarts\s*:\s*(\d+)',
        }
        
        for stat, pattern in solver_patterns.items():
            match = re.search(pattern, content, re.IGNORECASE)
            if match:
                result[stat] = int(match.group(1))
        
        # Determine SAT/UNSAT result
        if 'UNSATISFIABLE' in content:
            result['result'] = 'UNSAT'
        elif 'SATISFIABLE' in content and 'UNSATISFIABLE' not in content:
            result['result'] = 'SAT'
        # Any other case (INDETERMINATE, timeout, incomplete, etc.) stays as UNKNOWN
        
    except Exception as e:
        print(f"Warning: Partial parse of minisat log {log_file_path}: {e}")
        # Still return the partial result with UNKNOWN status
    
    return result


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
            'restarts': r'Restarts\s*:\s*(\d+)',
            # Speculation stats
            'spec_started': r'Spec\s+Started\s*:\s*(\d+)',
            'spec_finished': r'Spec\s+Finished\s*:\s*(\d+)'
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
            'heap_insert_cycles': r'Heap\s+Insert\s*:\s*[\d.]+%\s*\((\d+) cycles\)',
            'heap_bump_cycles': r'Heap\s+Bump\s*:\s*[\d.]+%\s*\((\d+) cycles\)',
            'restart_cycles': r'Restart\s*:\s*[\d.]+%\s*\((\d+) cycles\)',
            'total_counted_cycles': r'Total Counted:\s*(\d+) cycles'
        }
        
        for key, pattern in cycle_patterns.items():
            match = re.search(pattern, cycle_text)
            if match:
                cycle_stats[key] = int(match.group(1))
    
    return cycle_stats


def parse_histogram(content, section_title: str, key_prefix: str):
    """Generic histogram parser for sections with 'Total samples' and 'Bin' lines."""
    out = {}
    section = re.search(rf"=+\[\s*{re.escape(section_title)}\s*\]=+\n(.*?)\n=+", content, re.DOTALL)
    if not section:
        return out

    text = section.group(1)
    total_match = re.search(r"Total samples:\s*(\d+)", text)
    if total_match:
        out[f"{key_prefix}_total_samples"] = int(total_match.group(1))

    bins = {}
    # Ranged bins like [ 0- 0] or [ 3- 7]
    bin_pattern = r"Bin \[\s*(\d+)\s*-\s*(\d+)\s*\]:\s*(\d+)\s+samples \(([\d.]+)%\)"
    for m in re.finditer(bin_pattern, text):
        start = int(m.group(1))
        end = int(m.group(2))
        samples = int(m.group(3))
        pct = float(m.group(4))
        key = start if start == end else f"{start}-{end}"
        bins[key] = {"samples": samples, "percentage": pct}

    # Optional out-of-bounds
    oob = re.search(r"Out of bounds:\s*(\d+)\s+samples \(([\d.]+)%\)", text)
    if oob:
        bins["out_of_bounds"] = {"samples": int(oob.group(1)), "percentage": float(oob.group(2))}

    if bins:
        out[f"{key_prefix}_bins"] = bins
    return out


def parse_propagation_detail_statistics(content):
    """Parse the Propagation Detail Statistics section with per-activity % and cycles."""
    stats = {}
    section = re.search(r"=+\[\s*Propagation Detail Statistics\s*\]=+\n(.*?)\n=+", content, re.DOTALL)
    if not section:
        return stats

    text = section.group(1)
    # Match lines like: Label : 12.34% 	(12345 cycles)
    for line in text.splitlines():
        m = re.search(r"^\s*(.+?)\s*:\s*([\d.]+)%\s*\((\d+)\s*cycles\)\s*$", line)
        if not m:
            continue
        label = m.group(1).strip().lower()
        # normalize to snake_case
        key_base = 'prop_' + re.sub(r"[^a-z0-9]+", "_", label).strip('_')
        try:
            stats[f"{key_base}_pct"] = float(m.group(2))
            stats[f"{key_base}_cycles"] = int(m.group(3))
        except ValueError:
            # Skip malformed numbers
            continue
    return stats


def parse_directed_prefetcher_statistics(content):
    """Parse DirectedPrefetcher Statistics section if present."""
    stats = {}
    # Section starts with a simple header followed by key-value lines
    section = re.search(r"DirectedPrefetcher Statistics:\n(.*?)(?:\n={3,}|\n\[{3,}|\Z)", content, re.DOTALL)
    if not section:
        return stats
    text = section.group(1)

    m = re.search(r"Prefetches issued:\s*(\d+)", text)
    if m:
        stats['prefetches_issued'] = int(m.group(1))
    m = re.search(r"Prefetches used:\s*(\d+)", text)
    if m:
        stats['prefetches_used'] = int(m.group(1))
    m = re.search(r"Prefetches unused.*?:\s*(\d+)", text)
    if m:
        stats['prefetches_unused'] = int(m.group(1))
    m = re.search(r"Prefetch accuracy:\s*([\d.]+)%", text)
    if m:
        stats['prefetch_accuracy'] = float(m.group(1))
    return stats


def parse_stats_csv_for_prefetch(stats_csv_path: Path):
    """Extract last Prefetch_requests and Prefetch_drops from a stats CSV.

    Looks for rows with ComponentName starting with 'global_l1cache' and
    StatisticName equal to 'Prefetch_requests' or 'Prefetch_drops'. Returns
    values from Sum.u64 (column name in header) if present.
    """
    out = {}
    try:
        if not stats_csv_path.exists() or not stats_csv_path.is_file():
            return out

        last_requests = None
        last_drops = None
        with stats_csv_path.open('r', newline='') as f:
            reader = csv.DictReader(f)
            for row in reader:
                comp = row.get('ComponentName', '')
                name = row.get('StatisticName', '')
                if not comp.startswith('global_l1cache'):
                    continue
                if name == 'Prefetch_requests':
                    try:
                        last_requests = int(row.get('Sum.u64') or 0)
                    except (TypeError, ValueError):
                        pass
                elif name == 'Prefetch_drops':
                    try:
                        last_drops = int(row.get('Sum.u64') or 0)
                    except (TypeError, ValueError):
                        pass

        if last_requests is not None:
            out['l1_prefetch_requests'] = last_requests
        if last_drops is not None:
            out['l1_prefetch_drops'] = last_drops
    except Exception:
        # Ignore CSV parsing errors; keep parser resilient
        return out
    return out


def parse_log_file(log_file_path):
    """
    Parse a single log file and extract all relevant information.
    
    Automatically detects whether the log is from minisat or satsolver format
    and uses the appropriate parser.
    
    Returns a dictionary containing:
    - Basic info: test_case, result, variables, clauses
    - Memory: total_memory_bytes, total_memory_formatted  
    - Solver stats: decisions, propagations, conflicts, etc.
    - L1 cache stats: l1_total_requests, l1_total_miss_rate, l1_{component}_total, l1_{component}_miss_rate (satsolver only)
    - Fragmentation: heap_bytes, reserved_bytes, etc. (satsolver only)
    - Cycles: propagate_cycles, analyze_cycles, etc. (satsolver only)
    - Timing: sim_time_ms
    - Histogram data: watchers and variables histograms (satsolver only)
    
    Always returns a result dictionary, even for failed/timed-out runs.
    """
    try:
        with open(log_file_path, 'r', encoding='utf-8', errors='ignore') as f:
            content = f.read()
        
        # Detect log format
        log_format = detect_log_format(content)
        
        if log_format == 'minisat':
            # Use minisat parser
            return parse_minisat_log(log_file_path, content)
        elif log_format == 'satsolver':
            # Use satsolver parser
            return parse_satsolver_log(log_file_path, content)
        else:
            # Unknown format - try to extract minimal information
            print(f"Warning: Unknown log format for {log_file_path}, extracting basic info")
            filename = os.path.basename(log_file_path)
            test_case = re.sub(r'\.log$', '', filename)
            test_case = re.sub(r'_\d{8}_\d{6}$', '', test_case)
            test_case = re.sub(r'\.(xz|gz|bz2)$', '', test_case)
            
            return {
                'test_case': test_case,
                'result': 'UNKNOWN',
                'variables': 0,
                'clauses': 0,
                'total_memory_bytes': 0,
                'total_memory_formatted': '0 B',
                'sim_time_ms': 0.0,
            }
    
    except Exception as e:
        print(f"Error reading {log_file_path}: {e}")
        # Still return a minimal result
        filename = os.path.basename(log_file_path)
        test_case = re.sub(r'\.log$', '', filename)
        return {
            'test_case': test_case,
            'result': 'UNKNOWN',
            'variables': 0,
            'clauses': 0,
            'total_memory_bytes': 0,
            'total_memory_formatted': '0 B',
            'sim_time_ms': 0.0,
        }


def parse_satsolver_log(log_file_path, content):
    """
    Parse a satsolver format log file and extract all relevant information.
    
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
        # Extract test case name from filename
        filename = os.path.basename(log_file_path)
        test_case_match = re.match(r'(.+?)_(sat|unsat)_\d{8}_\d{6}\.log$', filename)
        if test_case_match:
            result['test_case'] = test_case_match.group(1)
            result['result'] = test_case_match.group(2).upper()
        else:
            result['test_case'] = os.path.splitext(filename)[0]
        
        # Extract variables and clauses
        problem_match = re.search(r'MAIN-> Problem: vars=(\d+) clauses=(\d+)', content)
        if problem_match:
            result['variables'] = int(problem_match.group(1))
            result['clauses'] = int(problem_match.group(2))

        # Memory usage aggregation
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
        for pattern, _ in memory_patterns:
            m = re.search(pattern, content)
            if m:
                total_bytes += int(m.group(1))
        result['total_memory_bytes'] = total_bytes
        result['total_memory_formatted'] = format_bytes(total_bytes)

        # Result sanity
        if 'UNSATISFIABLE' in content:
            result['result'] = 'UNSAT'
        elif 'SATISFIABLE' in content and 'UNSATISFIABLE' not in content:
            result['result'] = 'SAT'

        # Simulated time
        time_match = re.search(r'Simulation is complete, simulated time: ([\d.]+)\s*(\w+)', content)
        if time_match:
            time_val = float(time_match.group(1))
            time_unit = time_match.group(2)
            if time_unit == 'us':
                time_val *= 0.001
            elif time_unit == 's':
                time_val *= 1000
            result['sim_time_ms'] = time_val

        # Also attempt to pull prefetch requests/drops from matching .stats.csv
        try:
            stats_csv_path = Path(log_file_path).parent / f"{result['test_case']}.stats.csv"
            result.update(parse_stats_csv_for_prefetch(stats_csv_path))
        except Exception:
            pass

        result.update(parse_solver_statistics(content))
        result.update(parse_l1_cache_statistics(content))
        result.update(parse_clauses_fragmentation(content))
        result.update(parse_cycle_statistics(content))

        # Unified histogram parsing
        result.update(parse_histogram(content, 'Parallel Watchers Histogram', 'watchers'))
        result.update(parse_histogram(content, 'Parallel Variables Histogram', 'variables'))
        result.update(parse_histogram(content, 'Watchers Occupancy Histogram', 'watchers_occupancy'))
        result.update(parse_histogram(content, 'Watcher Blocks Visited Histogram', 'watcher_blocks_visited'))

        result.update(parse_propagation_detail_statistics(content))
        result.update(parse_directed_prefetcher_statistics(content))
        
        # Check result status - any case without SAT/UNSAT stays as UNKNOWN
        if not result['result'] or result['result'] not in ['SAT', 'UNSAT']:
            result['result'] = 'UNKNOWN'
                
    except Exception as e:
        print(f"Warning: Partial parse of satsolver log {log_file_path}: {e}")
        # Still return the partial result with UNKNOWN status
    
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
    
    for log_file in sorted(log_files):
        # Skip summary files if requested
        if exclude_summary and 'summary' in log_file.name.lower():
            continue
            
        result = parse_log_file(log_file)
        # Always include result, even if partial or failed
        if result:
            results.append(result)
    
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

