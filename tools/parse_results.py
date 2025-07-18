#!/usr/bin/env python3
"""
SAT Solver Result Parser

This script parses log files from SAT solver runs and generates CSV reports.

Usage: python parse_results.py <results_folder> [output_file]
"""

import sys
import csv
from pathlib import Path
from unified_parser import parse_log_directory, format_bytes


def write_csv_report(results, output_file):
    """Write detailed results to a CSV file."""
    # Sort results by total memory bytes before writing
    sorted_results = sorted(results, key=lambda x: x['total_memory_bytes'])
    
    # Define column order: basic info, solver stats, L1 totals, then L1 components, then fragmentation, cycles
    fieldnames = [
        'test_case', 'result', 'variables', 'clauses', 
        'total_memory_bytes', 'total_memory_formatted', 'sim_time_ms',
        # Solver statistics
        'decisions', 'propagations', 'conflicts', 'learned', 'removed',
        'db_reductions', 'assigns', 'unassigns', 'minimized', 'restarts',
        # L1 cache totals first
        'l1_total_requests', 'l1_total_miss_rate',
        # L1 cache by data structure
        'l1_heap_total', 'l1_heap_miss_rate', 'l1_heap_hits', 'l1_heap_misses',
        'l1_variables_total', 'l1_variables_miss_rate', 'l1_variables_hits', 'l1_variables_misses',
        'l1_watches_total', 'l1_watches_miss_rate', 'l1_watches_hits', 'l1_watches_misses',
        'l1_clauses_total', 'l1_clauses_miss_rate', 'l1_clauses_hits', 'l1_clauses_misses',
        'l1_varactivity_total', 'l1_varactivity_miss_rate', 'l1_varactivity_hits', 'l1_varactivity_misses',
        # Fragmentation statistics
        'heap_bytes', 'reserved_bytes', 'requested_bytes', 'allocated_bytes', 'wasted_bytes',
        'current_frag_percent', 'peak_frag_percent',
        # Cycle statistics
        'propagate_cycles', 'analyze_cycles', 'minimize_cycles', 'backtrack_cycles',
        'decision_cycles', 'reduce_db_cycles', 'restart_cycles', 'total_counted_cycles'
    ]
    
    with open(output_file, 'w', newline='') as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()
        
        for result in sorted_results:
            row = {}
            for field in fieldnames:
                row[field] = result.get(field, 0)
            writer.writerow(row)


def parse_results_folder(folder_path, output_file=None):
    """Parse all log files in the given folder and generate a report."""
    folder_path = Path(folder_path)
    
    if not folder_path.exists():
        print(f"Error: Folder {folder_path} does not exist")
        return
    
    # Parse all log files using unified parser
    results = parse_log_directory(folder_path, exclude_summary=True)
    
    if not results:
        print(f"No valid log files found in {folder_path}")
        return
    
    # Print parsing summary
    print(f"Successfully parsed: {len(results)} files")
    
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
