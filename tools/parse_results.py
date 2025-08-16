#!/usr/bin/env python3
"""
SAT Solver Result Parser

This script parses log files from SAT solver runs and generates CSV reports.

Enhancements:
- Reads matching <test_case>.stats.csv to extract L1 prefetch requests/drops
    from the last occurrence of rows starting with
    "global_l1cache,Prefetch_requests," and "global_l1cache,Prefetch_drops,"
    and records their Sum.u64 (column 7 in the CSV header) per test.
- Adds prefetch statistics and propagation detail statistics to the CSV.

Usage: python parse_results.py <results_folder> [output_file]
"""

import sys
import csv
from pathlib import Path
from unified_parser import parse_log_directory, format_bytes


# CSV prefetch parsing is handled in unified_parser now.


def write_csv_report(results, output_file):
    """Write detailed results to a CSV file with dynamic fields for extras."""
    # Sort results by total memory bytes before writing
    sorted_results = sorted(results, key=lambda x: x.get('total_memory_bytes', 0))

    # Base columns: basic info, solver stats, L1 totals, then L1 components (total+miss%), then cycles (+ percentages)
    base_fields = [
        'test_case', 'result', 'variables', 'clauses',
        'total_memory_bytes', 'total_memory_formatted', 'sim_time_ms',
        # Solver statistics
        'decisions', 'propagations', 'conflicts', 'learned', 'removed',
        'db_reductions', 'minimized', 'restarts',
        # L1 cache totals first
        'l1_total_requests', 'l1_total_miss_rate',
        # L1 cache by data structure
        'l1_heap_total', 'l1_heap_miss_rate',
        'l1_variables_total', 'l1_variables_miss_rate',
        'l1_watches_total', 'l1_watches_miss_rate',
        'l1_clauses_total', 'l1_clauses_miss_rate',
        'l1_varactivity_total', 'l1_varactivity_miss_rate',
        # Cycle statistics
        'propagate_cycles', 'analyze_cycles', 'minimize_cycles', 'backtrack_cycles',
        'decision_cycles', 'reduce_db_cycles', 'restart_cycles', 'total_counted_cycles',
        # Cycle percentages (computed)
        'propagate_cycles_pct', 'analyze_cycles_pct', 'minimize_cycles_pct', 'backtrack_cycles_pct',
        'decision_cycles_pct', 'reduce_db_cycles_pct', 'restart_cycles_pct'
    ]

    # Extra fixed fields: directed prefetcher stats and CSV prefetch requests/drops
    extra_fixed = [
        'prefetches_issued', 'prefetches_used', 'prefetches_unused', 'prefetch_accuracy',
    'l1_prefetch_requests', 'l1_prefetch_drops', 'l1_prefetch_drop_pct'
    ]

    # Dynamic propagation detail fields (union across results)
    prop_fields = sorted({k for r in results for k in r.keys() if k.startswith('prop_')})

    fieldnames = base_fields + extra_fixed + prop_fields

    with open(output_file, 'w', newline='') as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()

        cycle_names = [
            'propagate_cycles', 'analyze_cycles', 'minimize_cycles', 'backtrack_cycles',
            'decision_cycles', 'reduce_db_cycles', 'restart_cycles'
        ]

        for result in sorted_results:
            row = {field: result.get(field, 0) for field in fieldnames}

            # Compute cycle percentages if total_counted_cycles present
            total_cycles = result.get('total_counted_cycles', 0) or 0
            for name in cycle_names:
                pct_field = name.replace('_cycles', '_cycles_pct')
                cycles = result.get(name, 0) or 0
                row[pct_field] = (cycles / total_cycles * 100.0) if total_cycles > 0 else 0.0

            # Compute prefetch drop percentage if requests present
            req = result.get('l1_prefetch_requests', 0) or 0
            drops = result.get('l1_prefetch_drops', 0) or 0
            row['l1_prefetch_drop_pct'] = (drops / req * 100.0) if req > 0 else 0.0

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

    # unified_parser already enriches each result with CSV prefetch req/drops
    
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

    # Prefetch stats summary (DirectedPrefetcher + CSV requests/drops)
    prefetch_results = [r for r in results if any(k in r for k in ('prefetches_issued','prefetches_used','prefetches_unused','prefetch_accuracy','l1_prefetch_requests','l1_prefetch_drops'))]
    if prefetch_results:
        avg_acc = sum(r.get('prefetch_accuracy', 0.0) for r in prefetch_results if r.get('prefetch_accuracy') is not None) / max(1, sum(1 for r in prefetch_results if 'prefetch_accuracy' in r))
        avg_requests = sum(r.get('l1_prefetch_requests', 0) for r in prefetch_results) / len(prefetch_results)
        avg_drops = sum(r.get('l1_prefetch_drops', 0) for r in prefetch_results) / len(prefetch_results)
        print(f"\nPrefetch stats across problems: {len(prefetch_results)} with data")
        print(f"Average Prefetch accuracy: {avg_acc:.2f}% (if present)")
        print(f"Average L1 Prefetch requests (CSV): {avg_requests:.1f}")
        print(f"Average L1 Prefetch drops (CSV): {avg_drops:.1f}")

    # Propagation detail presence
    prop_keys = sorted({k for r in results for k in r if k.startswith('prop_')})
    if prop_keys:
        print(f"\nPropagation detail statistics collected for {sum(1 for r in results if any(k in r for k in prop_keys))} problems.")


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
