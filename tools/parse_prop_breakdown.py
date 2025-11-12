#!/usr/bin/env python3
"""
SAT Solver Propagation Breakdown Parser

This script parses log files from SAT solver runs and generates a CSV report
focused on propagation breakdown statistics.

The output CSV includes:
- Basic test info: test_case, result, variables, clauses
- Memory: total_memory_formatted
- Runtime: sim_time_ms
- Propagation breakdown in percentages (prop_*_pct)
- Propagation breakdown in absolute cycles (prop_*_cycles)

Usage: python parse_prop_breakdown.py <results_folder> [output_file]
"""

import sys
import csv
from pathlib import Path
from unified_parser import parse_log_directory, format_bytes


def write_prop_breakdown_csv(results, output_file):
    """Write propagation breakdown results to a CSV file.
    
    The CSV includes basic test info followed by all propagation detail
    statistics (both percentages and absolute cycles).
    """
    # Sort results by test case name
    sorted_results = sorted(results, key=lambda x: x.get('test_case', ''))
    
    # Base fields
    base_fields = [
        'test_case', 'result', 'variables', 'clauses',
        'total_memory_formatted', 'sim_time_ms'
    ]
    
    # Collect all propagation detail fields (prop_*_pct and prop_*_cycles)
    prop_fields = sorted({k for r in results for k in r.keys() if k.startswith('prop_')})
    
    # Separate percentage and cycles fields for better ordering
    prop_pct_fields = sorted([f for f in prop_fields if f.endswith('_pct')])
    prop_cycles_fields = sorted([f for f in prop_fields if f.endswith('_cycles')])
    
    # Cycle statistics fields (percentage only)
    cycle_fields_pct = [
        'propagate_cycles_pct', 'analyze_cycles_pct', 'minimize_cycles_pct',
        'backtrack_cycles_pct', 'decision_cycles_pct', 'reduce_db_cycles_pct',
        'restart_cycles_pct'
    ]
    
    fieldnames = base_fields + prop_pct_fields + cycle_fields_pct + prop_cycles_fields
    
    with open(output_file, 'w', newline='') as csvfile:
        writer = csv.DictWriter(csvfile, fieldnames=fieldnames)
        writer.writeheader()
        
        for result in sorted_results:
            row = {field: result.get(field, 0) for field in fieldnames}
            
            # Compute cycle percentages if total_counted_cycles present
            total_cycles = result.get('total_counted_cycles', 0) or 0
            if total_cycles > 0:
                cycle_names = [
                    'propagate_cycles', 'analyze_cycles', 'minimize_cycles',
                    'backtrack_cycles', 'decision_cycles', 'reduce_db_cycles',
                    'restart_cycles'
                ]
                for name in cycle_names:
                    pct_field = name.replace('_cycles', '_cycles_pct')
                    cycles = result.get(name, 0) or 0
                    row[pct_field] = (cycles / total_cycles * 100.0) if total_cycles > 0 else 0.0
            
            writer.writerow(row)


def parse_prop_breakdown_folder(folder_path, output_file=None):
    """Parse all log files in the given folder and generate a propagation breakdown report.
    
    Supports multi-seed folders: if the folder contains subfolders named
    seed*, each seed folder is parsed and results are averaged across seeds.
    
    Args:
        folder_path: Path to folder containing log files or seed* subfolders
        output_file: Optional path to write CSV report
    """
    folder_path = Path(folder_path)
    
    if not folder_path.exists():
        print(f"Error: Folder {folder_path} does not exist")
        return
    
    # Detect multi-seed layout
    seed_dirs = [p for p in sorted(folder_path.glob('seed*')) if p.is_dir()]
    
    def _avg_of(values):
        vals = [v for v in values if v is not None]
        if not vals:
            return 0
        return sum(vals) / len(vals)
    
    if seed_dirs:
        # Multi-seed mode: parse each seed and aggregate
        seed_results = []
        for sd in seed_dirs:
            res = parse_log_directory(sd, exclude_summary=True)
            if res:
                seed_results.append(res)
        
        if seed_results:
            print(f"Detected {len(seed_results)} seed folders under {folder_path}")
            
            # Build test_case union
            all_cases = set()
            for res in seed_results:
                for r in res:
                    all_cases.add(r.get('test_case', ''))
            
            # Map seed index -> {test_case -> result}
            seed_maps = []
            for res in seed_results:
                m = {r.get('test_case', ''): r for r in res}
                seed_maps.append(m)
            
            # Aggregate per test_case by averaging numeric fields across seeds
            aggregated_results = []
            
            # Fields to average (excluding test_case and result)
            numeric_fields = [
                'variables', 'clauses', 'total_memory_bytes', 'sim_time_ms'
            ]
            
            for case in sorted(all_cases):
                entries = [m[case] for m in seed_maps if case in m]
                if not entries:
                    continue
                
                agg = {'test_case': case}
                
                # Determine aggregate result label
                seed_labels = [e.get('result') for e in entries]
                
                unknown_count = sum(1 for lbl in seed_labels if lbl not in ('SAT', 'UNSAT'))
                finished_labels = [lbl for lbl in seed_labels if lbl in ('SAT', 'UNSAT')]
                base_label = None
                
                if finished_labels:
                    if all(lbl == 'SAT' for lbl in finished_labels):
                        base_label = 'SAT'
                    elif all(lbl == 'UNSAT' for lbl in finished_labels):
                        base_label = 'UNSAT'
                    else:
                        base_label = 'ERROR'
                
                if base_label is None:
                    agg['result'] = f"UNKNOWN {unknown_count}" if unknown_count > 0 else 'UNKNOWN'
                else:
                    agg['result'] = f"{base_label} {unknown_count}" if unknown_count > 0 else base_label
                
                # Finished entries for averaging (exclude UNKNOWN)
                finished = [e for e in entries if e.get('result') in ('SAT', 'UNSAT')]
                
                # Average numeric fields using finished only
                for key in numeric_fields:
                    vals = []
                    for e in finished:
                        v = e.get(key)
                        if v is None:
                            continue
                        try:
                            vals.append(float(v))
                        except (TypeError, ValueError):
                            continue
                    agg[key] = _avg_of(vals)
                
                # Memory formatted from averaged bytes
                agg['total_memory_formatted'] = format_bytes(int(agg.get('total_memory_bytes', 0) or 0))
                
                # Average cycle statistics fields (absolute cycles)
                cycle_keys = [
                    'propagate_cycles', 'analyze_cycles', 'minimize_cycles',
                    'backtrack_cycles', 'decision_cycles', 'reduce_db_cycles',
                    'restart_cycles', 'total_counted_cycles'
                ]
                for key in cycle_keys:
                    vals = []
                    for e in finished:
                        v = e.get(key)
                        if v is None:
                            continue
                        try:
                            vals.append(float(v))
                        except (TypeError, ValueError):
                            continue
                    agg[key] = _avg_of(vals)
                
                # Average all propagation detail fields (both _pct and _cycles)
                prop_keys = sorted({k for e in finished for k in e.keys() if k.startswith('prop_')})
                for key in prop_keys:
                    vals = []
                    for e in finished:
                        v = e.get(key)
                        if v is None:
                            continue
                        try:
                            vals.append(float(v))
                        except (TypeError, ValueError):
                            continue
                    agg[key] = _avg_of(vals)
                
                aggregated_results.append(agg)
            
            results = aggregated_results
            print(f"Successfully parsed and aggregated: {len(results)} test cases across {len(seed_results)} seeds")
        else:
            # No valid seed logs
            print(f"No valid log files found in seed folders under {folder_path}")
            return
    else:
        # Single-run folder
        results = parse_log_directory(folder_path, exclude_summary=True)
        
        if not results:
            print(f"No valid log files found in {folder_path}")
            return
        
        print(f"Successfully parsed: {len(results)} files")
    
    # Check if any propagation detail statistics exist
    prop_results = [r for r in results if any(k.startswith('prop_') for k in r.keys())]
    
    if not prop_results:
        print("\nWarning: No propagation detail statistics found in any log files")
        print("Make sure the logs contain 'Propagation Detail Statistics' section")
        return
    
    print(f"Found propagation breakdown data in {len(prop_results)} test cases")
    
    # Generate CSV output
    if output_file:
        write_prop_breakdown_csv(results, output_file)
        print(f"Propagation breakdown CSV written to: {output_file}")
    else:
        # Print sample to console if no output file specified
        print("\nSample propagation breakdown (first 5 test cases):")
        for i, r in enumerate(results[:5]):
            print(f"\n{r['test_case']} ({r['result']}):")
            print(f"  Variables: {r.get('variables', 0)}, Clauses: {r.get('clauses', 0)}")
            print(f"  Memory: {r.get('total_memory_formatted', 'N/A')}")
            print(f"  Sim time: {r.get('sim_time_ms', 0):.2f} ms")
            
            prop_keys = sorted([k for k in r.keys() if k.startswith('prop_') and k.endswith('_pct')])
            if prop_keys:
                print("  Propagation breakdown (%):")
                for key in prop_keys:
                    label = key.replace('prop_', '').replace('_pct', '').replace('_', ' ').title()
                    print(f"    {label}: {r.get(key, 0):.2f}%")


def main():
    if len(sys.argv) < 2:
        print("Usage: python parse_prop_breakdown.py <results_folder> [output_file]")
        print("Example: python parse_prop_breakdown.py ../runs/logs prop_breakdown.csv")
        sys.exit(1)
    
    results_folder = sys.argv[1]
    output_file = sys.argv[2] if len(sys.argv) > 2 else None
    
    parse_prop_breakdown_folder(results_folder, output_file)


if __name__ == "__main__":
    main()
