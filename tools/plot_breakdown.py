#!/usr/bin/env python3
"""
SAT Solver Breakdown Plotter

This script parses log files from SAT solver runs and generates PDF plots showing:
1. Total runtime breakdown (by cycle percentage)
2. Propagation detail breakdown (by cycle percentage within propagation)

The input takes a directory containing log files (not multi-seed).
Only finished tests (SAT/UNSAT) are included in the averaging.

Usage: python plot_breakdown.py <logs_folder> [output_pdf]
"""

import sys
from pathlib import Path
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from unified_parser import parse_log_directory


def compute_runtime_breakdown(results):
    """Compute average runtime breakdown across finished tests.
    
    Runtime breakdown includes:
    - Propagate
    - Analyze
    - Minimize
    - Backtrack
    - Priority Queue (decision + heap_insert + heap_bump)
    - Restart
    - Reduce DB
    
    Returns dict with component names and their average percentages.
    Percentages are capped to ensure they sum to 100%.
    """
    # Filter to finished tests only
    finished = [r for r in results if r.get('result') in ('SAT', 'UNSAT')]
    
    if not finished:
        return {}
    
    # Aggregate cycles across all finished tests
    total_propagate = 0
    total_analyze = 0
    total_minimize = 0
    total_backtrack = 0
    total_decision = 0
    total_reduce_db = 0
    total_restart = 0
    total_counted = 0
    
    # Also track heap operations from propagation detail if available
    total_heap_insert = 0
    total_heap_bump = 0
    
    for r in finished:
        total_propagate += r.get('propagate_cycles', 0) or 0
        total_analyze += r.get('analyze_cycles', 0) or 0
        total_minimize += r.get('minimize_cycles', 0) or 0
        total_backtrack += r.get('backtrack_cycles', 0) or 0
        total_decision += r.get('decision_cycles', 0) or 0
        total_reduce_db += r.get('reduce_db_cycles', 0) or 0
        total_restart += r.get('restart_cycles', 0) or 0
        total_counted += r.get('total_counted_cycles', 0) or 0
        
        # Check for heap operations in propagation detail
        total_heap_insert += r.get('prop_heap_insert_cycles', 0) or 0
        total_heap_bump += r.get('prop_heap_bump_cycles', 0) or 0
    
    if total_counted == 0:
        return {}
    
    # Compute priority queue as decision + heap operations
    total_priority_queue = total_decision + total_heap_insert + total_heap_bump
    
    # Build breakdown dict with raw cycles
    breakdown_cycles = {
        'Propagate': total_propagate,
        'Analyze': total_analyze,
        'Minimize': total_minimize,
        'Backtrack': total_backtrack,
        'Priority Queue': total_priority_queue,
        'Restart': total_restart,
        'Reduce DB': total_reduce_db,
    }
    
    # Compute raw percentages
    breakdown_pct = {k: (v / total_counted * 100.0) for k, v in breakdown_cycles.items()}
    
    # Cap to 100% total by normalizing
    total_pct = sum(breakdown_pct.values())
    if total_pct > 100.0:
        print("Warning: Runtime breakdown percentages exceed 100%, normalizing...")
        breakdown_pct = {k: (v / total_pct * 100.0) for k, v in breakdown_pct.items()}
    
    return breakdown_pct


def compute_propagation_breakdown(results):
    """Compute average propagation breakdown across finished tests.
    
    Propagation breakdown includes:
    - Insert Watchers
    - Polling for Busy
    - Read Clauses
    - Read Head Pointers (watchlist table)
    - Read Watcher Blocks (watchers)
    
    Returns dict with component names and their average percentages.
    Percentages are capped to ensure they sum to 100%.
    """
    # Filter to finished tests only
    finished = [r for r in results if r.get('result') in ('SAT', 'UNSAT')]
    
    if not finished:
        return {}
    
    # Aggregate cycles across all finished tests
    total_insert_watchers = 0
    total_polling = 0
    total_read_clauses = 0
    total_read_head_pointers = 0
    total_read_watcher_blocks = 0
    total_propagate = 0
    
    for r in finished:
        total_insert_watchers += r.get('prop_insert_watchers_cycles', 0) or 0
        total_polling += r.get('prop_polling_for_busy_cycles', 0) or 0
        total_read_clauses += r.get('prop_read_clauses_cycles', 0) or 0
        total_read_head_pointers += r.get('prop_read_head_pointers_cycles', 0) or 0
        total_read_watcher_blocks += r.get('prop_read_watcher_blocks_cycles', 0) or 0
        total_propagate += r.get('propagate_cycles', 0) or 0
    
    if total_propagate == 0:
        return {}
    
    # Build breakdown dict with raw cycles
    breakdown_cycles = {
        'Insert Watchers': total_insert_watchers,
        # 'Polling for Busy': total_polling,
        'Read Clauses': total_read_clauses,
        'Read Watchlist Table': total_read_head_pointers,
        'Read Watchers': total_read_watcher_blocks,
    }
    
    # Compute raw percentages
    breakdown_pct = {k: (v / total_propagate * 100.0) for k, v in breakdown_cycles.items()}
    
    return breakdown_pct


def plot_breakdowns(runtime_breakdown, prop_breakdown, output_pdf):
    """Create a PDF showing runtime breakdown with propagation subdivided.
    
    The propagation bar in the runtime breakdown is stacked to show its
    internal breakdown, while other components are shown as solid bars.
    All sorted by percentage in descending order.
    """
    # Sort by percentage (descending)
    runtime_sorted = sorted(runtime_breakdown.items(), key=lambda x: x[1], reverse=True)
    prop_sorted = sorted(prop_breakdown.items(), key=lambda x: x[1], reverse=True)
    
    # Create figure with single plot
    fig, ax = plt.subplots(1, 1, figsize=(12, 8))
    
    # Color palettes
    main_colors = plt.cm.Set3.colors
    prop_colors = plt.cm.Pastel1.colors
    
    # Plot: Total Runtime Breakdown with Propagation subdivided
    if runtime_sorted:
        labels = []
        values = []
        colors_list = []
        
        # Find propagate percentage for scaling sub-components
        propagate_pct = runtime_breakdown.get('Propagate', 0)
        
        y_pos = 0
        for i, (component, pct) in enumerate(runtime_sorted):
            if component == 'Propagate' and prop_sorted:
                # Stack propagation breakdown components
                left = 0
                for j, (prop_comp, prop_pct) in enumerate(prop_sorted):
                    # Scale propagation component percentage to total runtime percentage
                    scaled_pct = (prop_pct / 100.0) * propagate_pct
                    
                    bar = ax.barh(y_pos, scaled_pct, left=left, 
                                 color=prop_colors[j % len(prop_colors)],
                                 edgecolor='white', linewidth=1.5)
                    
                    # Add percentage label for first 3 largest components only
                    if j < 3:
                        ax.text(left + scaled_pct/2, y_pos, 
                               f'{scaled_pct:.1f}%',
                               ha='center', va='center', fontsize=16, fontweight='bold')
                    
                    left += scaled_pct
                
                labels.append(f'Propagate')
                # Add total propagate percentage at the end
                ax.text(propagate_pct + 0.5, y_pos, 
                       f'{propagate_pct:.1f}%', ha='left', va='center', 
                       fontsize=16, fontweight='bold')
            else:
                # Regular solid bar for non-propagate components
                bar = ax.barh(y_pos, pct, color=main_colors[i % len(main_colors)],
                             edgecolor='white', linewidth=1.5)
                labels.append(component)
                
                # Add percentage label
                ax.text(pct + 0.5, y_pos, f'{pct:.1f}%', 
                       ha='left', va='center', fontsize=16)
            
            y_pos += 1
        
        ax.set_yticks(range(len(labels)))
        ax.set_yticklabels(labels, fontsize=18)
        ax.set_xlabel('Percentage of Total Runtime (%)', fontsize=20, fontweight='bold')
        ax.tick_params(axis='x', which='major', labelsize=18)
        
        max_val = max([item[1] for item in runtime_sorted])
        ax.set_xlim(0, max_val * 1.15 if max_val else 100)
        ax.grid(axis='x', alpha=0.3, linestyle='--')
        
        # Add legend for propagation components if they exist
        if prop_sorted and propagate_pct > 0:
            legend_patches = [mpatches.Patch(color=prop_colors[j % len(prop_colors)], 
                                            label=prop_comp)
                            for j, (prop_comp, _) in enumerate(prop_sorted)]
            ax.legend(handles=legend_patches, loc='upper right', 
                     title='Propagation Components', fontsize=16, title_fontsize=18)
    else:
        ax.text(0.5, 0.5, 'No runtime breakdown data available', 
                ha='center', va='center', transform=ax.transAxes, fontsize=12)
        ax.set_xlim(0, 100)
    
    plt.tight_layout()
    plt.savefig(output_pdf, format='pdf', bbox_inches='tight', dpi=300)
    print(f"Plot saved to: {output_pdf}")
    plt.close()


def plot_breakdown_folder(folder_path, output_pdf=None):
    """Parse log files in folder and generate breakdown plots.
    
    Args:
        folder_path: Path to folder containing log files (not multi-seed)
        output_pdf: Optional path to write PDF plot
    """
    folder_path = Path(folder_path)
    
    if not folder_path.exists():
        print(f"Error: Folder {folder_path} does not exist")
        return
    
    # Parse all logs in the directory
    results = parse_log_directory(folder_path, exclude_summary=True)
    
    if not results:
        print(f"No valid log files found in {folder_path}")
        return
    
    # Filter to finished tests
    finished = [r for r in results if r.get('result') in ('SAT', 'UNSAT')]
    
    if not finished:
        print(f"No finished tests (SAT/UNSAT) found in {folder_path}")
        print(f"Total tests parsed: {len(results)}")
        return
    
    print(f"Successfully parsed: {len(results)} files")
    print(f"Finished tests (SAT/UNSAT): {len(finished)}")
    
    # Compute breakdowns
    runtime_breakdown = compute_runtime_breakdown(results)
    prop_breakdown = compute_propagation_breakdown(results)
    
    if not runtime_breakdown and not prop_breakdown:
        print("\nError: No breakdown statistics found in log files")
        print("Make sure the logs contain 'Cycle Statistics' and 'Propagation Detail Statistics' sections")
        return
    
    # Print breakdowns to console
    if runtime_breakdown:
        print("\n=== Total Runtime Breakdown ===")
        sorted_runtime = sorted(runtime_breakdown.items(), key=lambda x: x[1], reverse=True)
        for component, pct in sorted_runtime:
            print(f"  {component:20s}: {pct:6.2f}%")
        print(f"  {'Total':20s}: {sum(runtime_breakdown.values()):6.2f}%")
    
    if prop_breakdown:
        print("\n=== Propagation Detail Breakdown ===")
        sorted_prop = sorted(prop_breakdown.items(), key=lambda x: x[1], reverse=True)
        for component, pct in sorted_prop:
            print(f"  {component:20s}: {pct:6.2f}%")
        print(f"  {'Total':20s}: {sum(prop_breakdown.values()):6.2f}%")
    
    # Generate plot
    if output_pdf:
        plot_breakdowns(runtime_breakdown, prop_breakdown, output_pdf)
    else:
        print("\nNo output file specified. Use: python plot_breakdown.py <logs_folder> <output.pdf>")


def main():
    if len(sys.argv) < 2:
        print("Usage: python plot_breakdown.py <logs_folder> [output.pdf]")
        print("Example: python plot_breakdown.py ../runs/logs breakdown.pdf")
        sys.exit(1)
    
    logs_folder = sys.argv[1]
    output_pdf = sys.argv[2] if len(sys.argv) > 2 else None
    
    if output_pdf is None:
        # Auto-generate output filename based on input folder
        folder_name = Path(logs_folder).name
        # output_pdf = f"{folder_name}_breakdown.pdf"
        output_pdf = f"runtime_breakdown.pdf"
    
    plot_breakdown_folder(logs_folder, output_pdf)


if __name__ == "__main__":
    main()
