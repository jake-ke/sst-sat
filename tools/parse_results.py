#!/usr/bin/env python3
"""
SAT Solver Result Parser

This script parses log files from SAT solver runs and extracts:
- Test case name
- SAT/UNSAT result
- Number of variables and clauses
- Total memory usage across all components
- Solver statistics (decisions, conflicts, propagations, etc.)
- Cache profiler statistics (L1/L2/L3 cache hits/misses by component)

Usage: python parse_results.py <results_folder> [output_file]
"""

import os
import re
import sys
import csv
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


def parse_cache_statistics(content):
    """Parse cache profiler statistics for L1, L2, L3."""
    cache_stats = {}
    
    # Parse each cache level
    for level in ['L1', 'L2', 'L3']:
        cache_data = {}
        
        # Find cache section
        section_pattern = f'===+\\s*{level} Cache Profiler Statistics\\s*===+\\n(.*?)\\n===+'
        section_match = re.search(section_pattern, content, re.DOTALL)
        
        if section_match:
            section_text = section_match.group(1)
            
            # Parse total statistics first
            total_pattern = r'TOTAL\s*:\s*(\d+) hits,\s*(\d+) misses,\s*(\d+) total,\s*([\d.]+)% miss rate'
            total_match = re.search(total_pattern, section_text)
            if total_match:
                cache_data['total_requests'] = int(total_match.group(3))
                cache_data['total_miss_rate'] = float(total_match.group(4))
            
            # Parse component statistics (excluding ClaActivity)
            components = ['Heap', 'Variables', 'Watches', 'Clauses', 'VarActivity']
            for component in components:
                pattern = f'{component}\\s*:\\s*(\\d+) hits,\\s*(\\d+) misses,\\s*(\\d+) total,\\s*([\\d.]+)% miss rate'
                match = re.search(pattern, section_text)
                if match:
                    cache_data[f'{component.lower()}_total'] = int(match.group(3))
                    cache_data[f'{component.lower()}_miss_rate'] = float(match.group(4))
        
        if cache_data:
            cache_stats[level.lower()] = cache_data
    
    return cache_stats


def parse_log_file(log_file_path):
    """Parse a single log file and extract relevant information."""
    result = {
        'test_case': '',
        'result': '',
        'variables': 0,
        'clauses': 0,
        'total_memory_bytes': 0,
        'total_memory_formatted': ''
    }
    
    try:
        with open(log_file_path, 'r') as f:
            content = f.read()
        
        # Extract test case name from filename
        filename = os.path.basename(log_file_path)
        # Remove the timestamp and .log extension
        test_case_match = re.match(r'(.+?)_(sat|unsat)_\d{8}_\d{6}\.log$', filename)
        if test_case_match:
            result['test_case'] = test_case_match.group(1)
            result['result'] = test_case_match.group(2).upper()
        
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
        
        # Parse solver statistics
        solver_stats = parse_solver_statistics(content)
        result.update(solver_stats)
        
        # Parse cache statistics
        cache_stats = parse_cache_statistics(content)
        for level, stats in cache_stats.items():
            for key, value in stats.items():
                result[f'{level}_{key}'] = value
        
    except Exception as e:
        print(f"Error parsing {log_file_path}: {e}")
        return None
    
    return result


def parse_results_folder(folder_path, output_file=None):
    """Parse all log files in the given folder and generate a report."""
    folder_path = Path(folder_path)
    
    if not folder_path.exists():
        print(f"Error: Folder {folder_path} does not exist")
        return
    
    # Find all .log files
    log_files = list(folder_path.glob("*.log"))
    if not log_files:
        print(f"No .log files found in {folder_path}")
        return
    
    results = []
    failed_files = []
    
    for log_file in sorted(log_files):
        if log_file.name == 'summary_20250714_131834.log':
            continue  # Skip summary files
            
        result = parse_log_file(log_file)
        if result:
            results.append(result)
        else:
            failed_files.append(log_file.name)
    
    # Print parsing summary
    print(f"Successfully parsed: {len(results)} files")
    if failed_files:
        print(f"Failed to parse: {len(failed_files)} files")
    
    # Sort results by test case name
    results.sort(key=lambda x: x['test_case'])
    
    # Generate output
    if output_file:
        write_csv_report(results, output_file)
        print(f"CSV report written to: {output_file}")
    
    # Statistics
    sat_count = sum(1 for r in results if r['result'] == 'SAT')
    unsat_count = sum(1 for r in results if r['result'] == 'UNSAT')
    total_memory = sum(r['total_memory_bytes'] for r in results)
    avg_memory = total_memory / len(results) if results else 0
    total_decisions = sum(r.get('decisions', 0) for r in results)
    avg_decisions = total_decisions / len(results) if results else 0
    
    # Cache statistics
    l1_results = [r for r in results if r.get('l1_total_requests', 0) > 0]
    if l1_results:
        avg_l1_miss_rate = sum(r.get('l1_total_miss_rate', 0) for r in l1_results) / len(l1_results)
        
        # Calculate average miss rates per data structure
        components = ['heap', 'variables', 'watches', 'clauses', 'varactivity']
        component_stats = {}
        for comp in components:
            comp_results = [r for r in l1_results if r.get(f'l1_{comp}_total', 0) > 0]
            if comp_results:
                avg_miss_rate = sum(r.get(f'l1_{comp}_miss_rate', 0) for r in comp_results) / len(comp_results)
                component_stats[comp] = avg_miss_rate
    else:
        avg_l1_miss_rate = 0
        component_stats = {}
    
    print(f"\n=== STATISTICS SUMMARY ===")
    print(f"Total problems: {len(results)} (SAT: {sat_count}, UNSAT: {unsat_count})")
    print(f"Average memory per problem: {format_bytes(avg_memory)}")
    print(f"Average decisions per problem: {avg_decisions:.1f}")
    
    if l1_results:
        print(f"\nProblems with L1 cache data: {len(l1_results)}")
        print(f"Average L1 miss rate: {avg_l1_miss_rate:.2f}%")
        print("Average miss rates by data structure:")
        for comp, miss_rate in component_stats.items():
            print(f"  {comp.capitalize()}: {miss_rate:.2f}%")


def write_csv_report(results, output_file):
    """Write detailed results to a CSV file."""
    # Sort results by total memory bytes before writing
    sorted_results = sorted(results, key=lambda x: x['total_memory_bytes'])
    
    # Define column order: basic info, solver stats, L1 totals, then L1 components, then L2/L3
    fieldnames = [
        'test_case', 'result', 'variables', 'clauses', 
        'total_memory_bytes', 'total_memory_formatted',
        # Solver statistics
        'decisions', 'propagations', 'conflicts', 'learned', 'removed',
        'db_reductions', 'assigns', 'unassigns', 'minimized', 'restarts',
        # L1 cache totals first
        'l1_total_requests', 'l1_total_miss_rate',
        # L1 cache by data structure
        'l1_heap_total', 'l1_heap_miss_rate',
        'l1_variables_total', 'l1_variables_miss_rate', 
        'l1_watches_total', 'l1_watches_miss_rate',
        'l1_clauses_total', 'l1_clauses_miss_rate',
        'l1_varactivity_total', 'l1_varactivity_miss_rate',
        # L2 cache totals then components
        'l2_total_requests', 'l2_total_miss_rate',
        'l2_heap_total', 'l2_heap_miss_rate',
        'l2_variables_total', 'l2_variables_miss_rate',
        'l2_watches_total', 'l2_watches_miss_rate', 
        'l2_clauses_total', 'l2_clauses_miss_rate',
        'l2_varactivity_total', 'l2_varactivity_miss_rate',
        # L3 cache totals then components
        'l3_total_requests', 'l3_total_miss_rate',
        'l3_heap_total', 'l3_heap_miss_rate',
        'l3_variables_total', 'l3_variables_miss_rate',
        'l3_watches_total', 'l3_watches_miss_rate',
        'l3_clauses_total', 'l3_clauses_miss_rate', 
        'l3_varactivity_total', 'l3_varactivity_miss_rate'
    ]
    
    with open(output_file, 'w', newline='') as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()
        
        for result in sorted_results:
            row = {
                'test_case': result['test_case'],
                'result': result['result'],
                'variables': result['variables'],
                'clauses': result['clauses'],
                'total_memory_bytes': result['total_memory_bytes'],
                'total_memory_formatted': result['total_memory_formatted'],
                # Solver statistics
                'decisions': result.get('decisions', 0),
                'propagations': result.get('propagations', 0),
                'conflicts': result.get('conflicts', 0),
                'learned': result.get('learned', 0),
                'removed': result.get('removed', 0),
                'db_reductions': result.get('db_reductions', 0),
                'assigns': result.get('assigns', 0),
                'unassigns': result.get('unassigns', 0),
                'minimized': result.get('minimized', 0),
                'restarts': result.get('restarts', 0)
            }
            
            # Add cache statistics in the specified order
            for field in fieldnames[16:]:  # Skip the first 16 basic/solver fields
                row[field] = result.get(field, 0)
            
            writer.writerow(row)


def main():
    if len(sys.argv) < 2:
        print("Usage: python parse_results.py <results_folder> [output_file]")
        print("Example: python parse_results.py ../runs/logs results.csv")
        sys.exit(1)
    
    results_folder = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else None
    
    parse_results_folder(results_folder, output_file)


if __name__ == "__main__":
    main()
