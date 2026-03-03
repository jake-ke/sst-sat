#!/usr/bin/env python3
"""
SAT Solver Breakdown Plotter

This script parses log files from SAT solver runs and generates PDF plots showing:
1. Total runtime breakdown (by cycle percentage)
2. Propagation detail breakdown (by cycle percentage within propagation)
3. Optional accelerator comparison (side-by-side)

The input takes a directory containing log files (not multi-seed).
Only finished tests (SAT/UNSAT) are included in the averaging.

Usage: python plot_breakdown.py <logs_folder> [output_pdf] [--accel <accel_folder>]
"""

import sys
import argparse
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
    # Include all tests (SAT, UNSAT, and UNKNOWN)
    finished = [r for r in results if r.get('result') in ('SAT', 'UNSAT', 'UNKNOWN')]
    
    if not finished:
        return {}
    
    # Initialize lists to collect percentages for each test
    component_names = ['Propagate', 'Analyze', 'Minimize', 'Backtrack', 
                      'Priority Queue', 'Restart', 'Deletion']
    component_percentages = {name: [] for name in component_names}
    
    # Compute percentage for each test individually
    for r in finished:
        total_counted = r.get('total_counted_cycles', 0) or 0
        
        if total_counted == 0:
            continue
        
        # Get cycles for each component
        propagate = r.get('propagate_cycles', 0) or 0
        analyze = r.get('analyze_cycles', 0) or 0
        minimize = r.get('minimize_cycles', 0) or 0
        backtrack = r.get('backtrack_cycles', 0) or 0
        decision = r.get('decision_cycles', 0) or 0
        reduce_db = r.get('reduce_db_cycles', 0) or 0
        restart = r.get('restart_cycles', 0) or 0
        
        # Get heap operations from cycle statistics (not propagation detail)
        heap_insert = r.get('heap_insert_cycles', 0) or 0
        heap_bump = r.get('heap_bump_cycles', 0) or 0
        
        # Compute priority queue as decision + heap operations
        priority_queue = decision + heap_insert + heap_bump
        
        # Compute percentage for this test
        component_percentages['Propagate'].append(propagate / total_counted * 100.0)
        component_percentages['Analyze'].append(analyze / total_counted * 100.0)
        component_percentages['Minimize'].append(minimize / total_counted * 100.0)
        component_percentages['Backtrack'].append(backtrack / total_counted * 100.0)
        component_percentages['Priority Queue'].append(priority_queue / total_counted * 100.0)
        component_percentages['Restart'].append(restart / total_counted * 100.0)
        component_percentages['Deletion'].append(reduce_db / total_counted * 100.0)
    
    # Compute average percentage across all tests
    breakdown_pct = {}
    for name in component_names:
        if component_percentages[name]:
            breakdown_pct[name] = sum(component_percentages[name]) / len(component_percentages[name])
        else:
            breakdown_pct[name] = 0.0
    
    if not any(breakdown_pct.values()):
        return {}, {}
    
    return breakdown_pct, component_percentages


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
    # Include all tests (SAT, UNSAT, and UNKNOWN)
    finished = [r for r in results if r.get('result') in ('SAT', 'UNSAT', 'UNKNOWN')]
    
    if not finished:
        return {}
    
    # Initialize lists to collect percentages for each test
    component_names = ['Insert Watchers', 'Read Clauses', 
                      'Read Watchlist Table', 'Read Watchers']
    component_percentages = {name: [] for name in component_names}
    
    # Compute percentage for each test individually
    for r in finished:
        total_propagate = r.get('propagate_cycles', 0) or 0
        
        if total_propagate == 0:
            continue
        
        # Get cycles for each propagation component
        insert_watchers = r.get('prop_insert_watchers_cycles', 0) or 0
        read_clauses = r.get('prop_read_clauses_cycles', 0) or 0
        read_head_pointers = r.get('prop_read_head_pointers_cycles', 0) or 0
        read_watcher_blocks = r.get('prop_read_watcher_blocks_cycles', 0) or 0
        
        # Compute percentage for this test
        component_percentages['Insert Watchers'].append(insert_watchers / total_propagate * 100.0)
        component_percentages['Read Clauses'].append(read_clauses / total_propagate * 100.0)
        component_percentages['Read Watchlist Table'].append(read_head_pointers / total_propagate * 100.0)
        component_percentages['Read Watchers'].append(read_watcher_blocks / total_propagate * 100.0)
    
    # Compute average percentage across all tests
    breakdown_pct = {}
    for name in component_names:
        if component_percentages[name]:
            breakdown_pct[name] = sum(component_percentages[name]) / len(component_percentages[name])
        else:
            breakdown_pct[name] = 0.0
    
    if not any(breakdown_pct.values()):
        return {}
    
    return breakdown_pct


def _plot_single_breakdown(ax, runtime_breakdown, runtime_raw, prop_breakdown,
                           show_prop_stacked=True, show_pq_boxplot=True, title=None):
    """Render a single runtime breakdown onto the given axes.

    Args:
        ax: matplotlib Axes object to draw on
        runtime_breakdown: dict of component -> average percentage
        runtime_raw: dict of component -> list of per-test percentages
        prop_breakdown: dict of propagation sub-component -> percentage
        show_prop_stacked: if True, Propagate bar shows stacked sub-components
        show_pq_boxplot: if True, Priority Queue uses box-and-whisker plot
        title: optional subplot title

    Returns:
        max_val: the largest component percentage (for shared x-axis computation)
    """
    runtime_sorted = sorted(runtime_breakdown.items(), key=lambda x: x[1], reverse=True)
    prop_sorted = sorted(prop_breakdown.items(), key=lambda x: x[1], reverse=True)

    bar_color = plt.cm.Set3.colors[0]
    prop_colors = plt.cm.Pastel1.colors

    if runtime_sorted:
        labels = []
        propagate_pct = runtime_breakdown.get('Propagate', 0)

        y_pos = 0
        for i, (component, pct) in enumerate(runtime_sorted):
            if component == 'Propagate' and prop_sorted and show_prop_stacked:
                # Stack propagation breakdown components
                left = 0
                for j, (prop_comp, prop_pct) in enumerate(prop_sorted):
                    scaled_pct = (prop_pct / 100.0) * propagate_pct

                    ax.barh(y_pos, scaled_pct, left=left,
                            color=prop_colors[j % len(prop_colors)],
                            edgecolor='white', linewidth=1.5)

                    if j < 3:
                        ax.text(left + scaled_pct/2, y_pos,
                               f'{scaled_pct:.1f}%',
                               ha='center', va='center', fontsize=22)

                    left += scaled_pct

                labels.append('Propagate')
                ax.text(propagate_pct + 0.5, y_pos,
                       f'{propagate_pct:.1f}%', ha='left', va='center', fontsize=22)
            elif component == 'Priority Queue' and runtime_raw.get('Priority Queue') and show_pq_boxplot:
                # Box and whisker plot for Priority Queue
                pq_data = runtime_raw['Priority Queue']
                ax.boxplot([pq_data], positions=[y_pos], vert=False, widths=0.6,
                           patch_artist=True, showmeans=True,
                           boxprops=dict(facecolor=bar_color, alpha=0.7),
                           medianprops=dict(color='red', linewidth=2.5),
                           meanprops=dict(marker='D', markerfacecolor='black', markersize=10))
                labels.append(component)

                ax.text(pct + 0.5, y_pos, f'{pct:.1f}%',
                       ha='left', va='center', fontsize=22)
            else:
                # Regular solid bar
                ax.barh(y_pos, pct, color=bar_color,
                        edgecolor='white', linewidth=1.5)
                labels.append(component)

                ax.text(pct + 0.5, y_pos, f'{pct:.1f}%',
                       ha='left', va='center', fontsize=22)

            y_pos += 1

        ax.set_yticks(range(len(labels)))
        ax.set_yticklabels(labels, fontsize=24)
        ax.set_xlabel('Percentage of Total Runtime (%)', fontsize=26)
        ax.tick_params(axis='x', which='major', labelsize=22)
        ax.grid(axis='x', alpha=0.3, linestyle='--')

        if title:
            ax.set_title(title, fontsize=26, fontweight='bold')

        # Add legend for propagation components if they exist
        if prop_sorted and propagate_pct > 0 and show_prop_stacked:
            legend_patches = [mpatches.Patch(color=prop_colors[j % len(prop_colors)],
                                            label=prop_comp)
                            for j, (prop_comp, _) in enumerate(prop_sorted)]
            ax.legend(handles=legend_patches, loc='upper right',
                     title='Propagation Components', fontsize=20, title_fontsize=22)

        return max(item[1] for item in runtime_sorted)
    else:
        ax.text(0.5, 0.5, 'No runtime breakdown data available',
                ha='center', va='center', transform=ax.transAxes, fontsize=18)
        return 0


def plot_breakdowns(runtime_breakdown, runtime_raw, prop_breakdown, output_pdf):
    """Create a PDF showing runtime breakdown with propagation subdivided.

    The propagation bar in the runtime breakdown is stacked to show its
    internal breakdown, while other components are shown as solid bars.
    Priority Queue shows a box and whisker plot.
    All sorted by percentage in descending order.
    """
    fig, ax = plt.subplots(1, 1, figsize=(12, 8))

    max_val = _plot_single_breakdown(
        ax, runtime_breakdown, runtime_raw, prop_breakdown,
        show_prop_stacked=True, show_pq_boxplot=True
    )
    ax.set_xlim(0, max_val * 1.15 if max_val else 100)

    plt.tight_layout()
    plt.savefig(output_pdf, format='pdf', bbox_inches='tight', dpi=300)
    print(f"Plot saved to: {output_pdf}")
    plt.close()


def plot_combined_breakdown(baseline_data, accel_data, output_pdf):
    """Create a grouped bar chart comparing baseline and SATBlast.

    Uses the same style/colors as the original plot. For each component the
    baseline bar is on top and the SATBlast bar is directly below it.
    Baseline Propagate is stacked with sub-components; baseline Priority Queue
    uses a box-and-whisker. SATBlast uses solid bars for both.
    Y-axis order matches the original plot (baseline percentage descending).
    """
    base_bd = baseline_data['runtime_breakdown']
    base_raw = baseline_data['runtime_raw']
    prop_bd = baseline_data['prop_breakdown']
    accel_bd = accel_data['runtime_breakdown']

    # Same order as original plot: baseline percentage descending
    runtime_sorted = sorted(base_bd.items(), key=lambda x: x[1], reverse=True)
    prop_sorted = sorted(prop_bd.items(), key=lambda x: x[1], reverse=True)

    base_bar_color = plt.cm.Set3.colors[0]
    prop_colors = plt.cm.Pastel1.colors
    accel_color = '#5B9BD5'

    fig, ax = plt.subplots(1, 1, figsize=(12, 10))

    bar_height = 0.35
    labels = []

    y_pos = 0
    for i, (component, base_pct) in enumerate(runtime_sorted):
        y_baseline = y_pos + bar_height / 2    # top bar
        y_accel = y_pos - bar_height / 2        # bottom bar
        accel_pct = accel_bd.get(component, 0)

        # --- Baseline bar (top) ---
        propagate_pct = base_bd.get('Propagate', 0)
        if component == 'Propagate' and prop_sorted:
            # Stacked propagation sub-components
            left = 0
            for j, (prop_comp, prop_pct) in enumerate(prop_sorted):
                scaled_pct = (prop_pct / 100.0) * propagate_pct
                ax.barh(y_baseline, scaled_pct, height=bar_height, left=left,
                        color=prop_colors[j % len(prop_colors)],
                        edgecolor='white', linewidth=1.5)
                if j < 3:
                    ax.text(left + scaled_pct / 2, y_baseline,
                            f'{scaled_pct:.1f}%',
                            ha='center', va='center', fontsize=18)
                left += scaled_pct
            ax.text(propagate_pct + 0.5, y_baseline,
                    f'{propagate_pct:.1f}%', ha='left', va='center', fontsize=18)
        elif component == 'Priority Queue' and base_raw.get('Priority Queue'):
            pq_data = base_raw['Priority Queue']
            ax.boxplot([pq_data], positions=[y_baseline], vert=False,
                       widths=bar_height * 0.9,
                       patch_artist=True, showmeans=True,
                       boxprops=dict(facecolor=base_bar_color, alpha=0.7),
                       medianprops=dict(color='red', linewidth=2.5),
                       meanprops=dict(marker='D', markerfacecolor='black', markersize=8))
            ax.text(base_pct + 0.5, y_baseline, f'{base_pct:.1f}%',
                    ha='left', va='center', fontsize=18)
        else:
            ax.barh(y_baseline, base_pct, height=bar_height,
                    color=base_bar_color, edgecolor='white', linewidth=1.5)
            ax.text(base_pct + 0.5, y_baseline, f'{base_pct:.1f}%',
                    ha='left', va='center', fontsize=18)

        # --- SATBlast bar (bottom) - uniform color ---
        ax.barh(y_accel, accel_pct, height=bar_height,
                color=accel_color, edgecolor='white', linewidth=1.5)
        ax.text(accel_pct + 0.5, y_accel, f'{accel_pct:.1f}%',
                ha='left', va='center', fontsize=18)

        labels.append(component)
        y_pos += 1

    ax.set_yticks(range(len(labels)))
    ax.set_yticklabels(labels, fontsize=24)
    ax.set_xlabel('Percentage of Total Runtime (%)', fontsize=26)
    ax.tick_params(axis='x', which='major', labelsize=22)
    ax.grid(axis='x', alpha=0.3, linestyle='--')

    max_val = max(v for _, v in runtime_sorted)
    max_accel = max(accel_bd.get(c, 0) for c, _ in runtime_sorted)
    overall_max = max(max_val, max_accel)
    ax.set_xlim(0, overall_max * 1.15 if overall_max else 100)

    # Two separate legends: propagation sub-components and baseline/SATBlast
    if prop_sorted and propagate_pct > 0:
        prop_patches = [mpatches.Patch(color=prop_colors[j % len(prop_colors)], label=pc)
                        for j, (pc, _) in enumerate(prop_sorted)]
        prop_legend = ax.legend(handles=prop_patches, loc='upper right',
                               title='Propagation Components', fontsize=20, title_fontsize=22)
        ax.add_artist(prop_legend)

    config_patches = [
        mpatches.Patch(color=base_bar_color, label='Baseline'),
        mpatches.Patch(color=accel_color, label='SATBlast'),
    ]
    ax.legend(handles=config_patches, loc='upper right',
             bbox_to_anchor=(1.0, 0.72), fontsize=20)

    plt.tight_layout()
    plt.savefig(output_pdf, format='pdf', bbox_inches='tight', dpi=300)
    print(f"Combined plot saved to: {output_pdf}")
    plt.close()


def _parse_folder(folder_path, label=""):
    """Parse log files in folder and compute breakdowns.

    Args:
        folder_path: Path to folder containing log files
        label: Optional label for console output prefix

    Returns:
        dict with 'runtime_breakdown', 'runtime_raw', 'prop_breakdown',
        or None if parsing fails.
    """
    folder_path = Path(folder_path)

    if not folder_path.exists():
        print(f"Error: Folder {folder_path} does not exist")
        return None

    results = parse_log_directory(folder_path, exclude_summary=True)

    if not results:
        print(f"No valid log files found in {folder_path}")
        return None

    finished = [r for r in results if r.get('result') in ('SAT', 'UNSAT', 'UNKNOWN')]

    if not finished:
        print(f"No finished tests (SAT/UNSAT/UNKNOWN) found in {folder_path}")
        print(f"Total tests parsed: {len(results)}")
        return None

    prefix = f"[{label}] " if label else ""
    print(f"{prefix}Successfully parsed: {len(results)} files")
    print(f"{prefix}Finished tests (SAT/UNSAT/UNKNOWN): {len(finished)}")

    runtime_breakdown, runtime_raw = compute_runtime_breakdown(results)
    prop_breakdown = compute_propagation_breakdown(results)

    if not runtime_breakdown and not prop_breakdown:
        print(f"{prefix}Error: No breakdown statistics found in log files")
        print("Make sure the logs contain 'Cycle Statistics' and 'Propagation Detail Statistics' sections")
        return None

    if runtime_breakdown:
        print(f"\n=== {label + ' ' if label else ''}Total Runtime Breakdown ===")
        for component, pct in sorted(runtime_breakdown.items(), key=lambda x: x[1], reverse=True):
            print(f"  {component:20s}: {pct:6.2f}%")
        print(f"  {'Total':20s}: {sum(runtime_breakdown.values()):6.2f}%")

    if prop_breakdown:
        print(f"\n=== {label + ' ' if label else ''}Propagation Detail Breakdown ===")
        for component, pct in sorted(prop_breakdown.items(), key=lambda x: x[1], reverse=True):
            print(f"  {component:20s}: {pct:6.2f}%")
        print(f"  {'Total':20s}: {sum(prop_breakdown.values()):6.2f}%")

    return {
        'runtime_breakdown': runtime_breakdown,
        'runtime_raw': runtime_raw,
        'prop_breakdown': prop_breakdown,
    }


def plot_breakdown_folder(folder_path, output_pdf=None, accel_data=None):
    """Parse log files in folder and generate breakdown plots.

    Args:
        folder_path: Path to folder containing log files (not multi-seed)
        output_pdf: Optional path to write PDF plot
        accel_data: Optional dict from _parse_folder() for accelerator comparison
    """
    baseline_data = _parse_folder(folder_path, label="Baseline" if accel_data else "")

    if baseline_data is None:
        return

    if output_pdf:
        plot_breakdowns(
            baseline_data['runtime_breakdown'],
            baseline_data['runtime_raw'],
            baseline_data['prop_breakdown'],
            output_pdf,
        )

        # Generate additional combined plot when accelerator data is provided
        if accel_data:
            pdf_path = Path(output_pdf)
            combined_pdf = str(pdf_path.parent / f"{pdf_path.stem}_combined{pdf_path.suffix}")
            plot_combined_breakdown(baseline_data, accel_data, combined_pdf)
    else:
        print("\nNo output file specified. Use: python plot_breakdown.py <logs_folder> <output.pdf>")


def main():
    parser = argparse.ArgumentParser(
        description='SAT Solver Runtime Breakdown Plotter',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='Examples:\n'
               '  %(prog)s logs/baseline\n'
               '  %(prog)s logs/baseline breakdown.pdf\n'
               '  %(prog)s logs/baseline breakdown.pdf --accel logs/accelerator\n'
    )

    parser.add_argument('logs_folder', help='Path to folder containing log files')
    parser.add_argument('output_pdf', nargs='?', default=None,
                       help='Output PDF file path (default: runtime_breakdown.pdf)')
    parser.add_argument('--accel', metavar='ACCEL_FOLDER', default=None,
                       help='Path to accelerator logs folder for side-by-side comparison')

    args = parser.parse_args()

    if args.output_pdf is None:
        args.output_pdf = "runtime_breakdown.pdf"

    accel_data = None
    if args.accel:
        accel_data = _parse_folder(args.accel, label="Accelerator")

    plot_breakdown_folder(args.logs_folder, args.output_pdf, accel_data=accel_data)


if __name__ == "__main__":
    main()
