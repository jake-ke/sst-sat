#!/usr/bin/env python3
"""
Large Tests Per-Test Speedup Plotter

Standalone script for generating per-test speedup charts for large SAT benchmarks.
Hardcodes the MANUAL_EXCLUSIVE_TESTS list and only generates the per-test speedup chart.
Borrows functions from plot_comparison.py.

Usage:
    python plot_large_tests.py <folder1> <folder2> [...] --names "Baseline" "MiniSAT" "SATBlast"
"""

import sys
import argparse
from pathlib import Path
from collections import OrderedDict
import matplotlib.pyplot as plt
import matplotlib.backends.backend_pdf

# Import shared functions from plot_comparison
import plot_comparison
from plot_comparison import (
    compute_metrics_for_folder,
    get_shared_test_set,
    compute_par2_on_shared_set,
    compute_geomean_speedups,
    get_folder_colors,
    dump_comparison_csv,
    FIG_SIZE,
)

# Large tests: >120k variables or >640k clauses
MANUAL_EXCLUSIVE_TESTS = [
    "e185ebbd92c23ab460a3d29046eccf1d-group_mulr.cnf",
    "7b5895f110a6aa5a5ac5d5a6eb3fd322-g2-ak128modasbg1sbisc.cnf",
    "7aa3b29dde431cdacf17b2fb9a10afe4-Mario-t-hard-2_c18.cnf",
    "fce130da1efd25fa9b12e59620a4146b-g2-ak128diagobg1btaig.cnf",
    "91c69a1dedfa5f2b3779e38c48356593-Problem14_label20_true-unreach-call.c.cnf",
    "1427381a809c64c721838894ece6756d-shuffling-2-s25242449-of-bench-sat04-727.used-as.sat04-753.cnf",
]


def plot_per_test_speedups(folder_metrics, shared_tests, timeout_seconds, output_dir, baseline_name, geomean_results, pdf_basename='per_test_speedups', large_fonts=False):
    """Plot per-test speedup bars with legend above the plot.
    Modified from plot_comparison.py: legend moved above axes to avoid overlap with
    broken-axis clipped bars (cutoff at 15).
    """
    font_scale = 3.0 if large_fonts else 1.0
    per_test_fig_size = (FIG_SIZE[0] * font_scale, FIG_SIZE[1] / 2 * font_scale) if large_fonts else (FIG_SIZE[0], FIG_SIZE[1] / 2)

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    pdf_path = output_dir / f'{pdf_basename}.pdf'

    timeout_ms = timeout_seconds * 1000.0
    penalty_ms = 2 * timeout_ms

    folder_names = list(folder_metrics.keys())
    if baseline_name not in folder_names:
        print("Warning: baseline not in folder_metrics for per-test speedups")
        return

    ordered_tests = shared_tests

    test_times = {}
    for tc in ordered_tests:
        test_times[tc] = {}
        for folder_name, metrics in folder_metrics.items():
            for r in metrics['results']:
                if r.get('test_case') == tc:
                    try:
                        sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
                    except (TypeError, ValueError):
                        sim_ms = penalty_ms
                    result = r.get('result')
                    t_eff = sim_ms if (result in ('SAT', 'UNSAT') and sim_ms <= timeout_ms) else penalty_ms
                    test_times[tc][folder_name] = max(t_eff, 1e-6)
                    break

    test_labels = [tc[:5] for tc in ordered_tests]
    test_labels.append('gmean')

    num_folders = len(folder_names)
    num_groups = len(test_labels)

    speedup_matrix = []
    for tc in ordered_tests:
        row = []
        t_base = test_times[tc].get(baseline_name, penalty_ms)
        for folder_name in folder_names:
            t_cfg = test_times[tc].get(folder_name, penalty_ms)
            speedup = t_base / t_cfg if t_cfg > 0 else 0.0
            row.append(speedup)
        speedup_matrix.append(row)

    geomean_row = []
    for folder_name in folder_names:
        if folder_name == baseline_name:
            geomean_row.append(1.0)
        else:
            geomean_val = geomean_results.get(folder_name, None)
            geomean_row.append(geomean_val if geomean_val is not None else 0.0)
    speedup_matrix.append(geomean_row)

    max_speedup = max(max(row) for row in speedup_matrix)
    use_broken_axis = max_speedup > 20

    with matplotlib.backends.backend_pdf.PdfPages(pdf_path) as pdf:
        if use_broken_axis:
            fig, ax_bottom = plt.subplots(figsize=per_test_fig_size)
            ax_bottom.set_ylim(0, 20)
        else:
            fig, ax_bottom = plt.subplots(figsize=per_test_fig_size)
            ax_bottom.set_ylim(0, max_speedup * 1.2)

        bar_width = 0.9 / num_folders
        x_base = range(num_groups)

        folder_colors = get_folder_colors(folder_names)

        bars_list = []
        for folder_idx, folder_name in enumerate(folder_names):
            x_positions = [x + folder_idx * bar_width for x in x_base]
            values = [speedup_matrix[group_idx][folder_idx] for group_idx in range(num_groups)]

            display_values = []
            for v in values:
                if use_broken_axis and v > 20:
                    display_values.append(20)
                else:
                    display_values.append(v)

            bars = ax_bottom.bar(x_positions, display_values, bar_width,
                   label=folder_name,
                   color=folder_colors[folder_idx],
                   alpha=0.9, edgecolor='black', linewidth=0.5)
            bars_list.append((bars, values, x_positions))

        # Label bars that exceed the limit
        if use_broken_axis:
            for folder_idx, (bars, values, x_positions) in enumerate(bars_list):
                for bar_idx, (bar, val, x_pos) in enumerate(zip(bars, values, x_positions)):
                    if val > 20:
                        ax_bottom.text(x_pos, 19.5, f'{val:.0f}', ha='center', va='top',
                                       fontsize=int(16 * font_scale), rotation=0, color='black')

        # Grid
        ax_bottom.yaxis.set_major_locator(plt.MultipleLocator(5))
        ax_bottom.yaxis.set_minor_locator(plt.MultipleLocator(1))
        ax_bottom.grid(axis='y', which='major', alpha=0.6, linestyle='-', linewidth=1.2)
        ax_bottom.grid(axis='y', which='minor', alpha=0.4, linestyle='-', linewidth=0.8)
        ax_bottom.set_axisbelow(True)

        # Red dashed line at 1.0
        ax_bottom.axhline(y=1.0, color='darkred', linestyle='--', linewidth=1.5, alpha=0.8, zorder=3)

        ax_bottom.tick_params(axis='y', labelsize=int(22 * font_scale))

        ax_bottom.set_ylabel('Speedup', fontsize=int(24 * font_scale))
        ax_bottom.set_xticks([x + bar_width * (num_folders - 1) / 2 for x in x_base])
        ax_bottom.set_xticklabels(test_labels, fontsize=int(22 * font_scale), ha='center')

        # Legend ABOVE the plot (outside axes) to avoid overlap with clipped bars
        ax_bottom.legend(loc='lower center', fontsize=int(22 * font_scale), frameon=False,
                        ncol=min(num_folders, 5), bbox_to_anchor=(0.5, 0.98),
                        handlelength=1.0, handletextpad=0.5, columnspacing=1.0)

        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)

    print(f"Per-test speedup chart saved to: {pdf_path}")


def main():
    parser = argparse.ArgumentParser(
        description='Generate per-test speedup chart for large SAT benchmarks',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Example:
  %(prog)s ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 \\
            ../sat-isca26-data/stereo_minisat_logs/ \\
            ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ \\
            --names "Baseline" "MiniSAT" "SATBlast"
        """
    )

    parser.add_argument('folders', nargs='+', help='Input folders or raw text files to compare (in display order)')
    parser.add_argument('--names', nargs='+',
                       help='Custom names for each folder (must match number of folders)')
    parser.add_argument('--timeout', type=float, default=36,
                       help='Timeout in seconds for PAR-2 calculation (default: 36)')
    parser.add_argument('--output-dir', default='results/large_tests',
                       help='Output directory for plots (default: results/large_tests)')
    parser.add_argument('--errors-as-timeout', action='store_true',
                       help='Treat ERROR/UNKNOWN as timeout (1×timeout penalty) instead of excluding them')
    parser.add_argument('--large-fonts', action='store_true',
                       help='Use much larger font sizes (useful when scaling down for side-by-side comparisons)')
    parser.add_argument('--normalize-sataccel', action='store_true',
                       help='Divide all runtimes in .txt file inputs by 4 (normalize SatAccel clock scaling)')

    args = parser.parse_args()

    if len(args.folders) < 2:
        print("Error: Need at least 2 folders to compare")
        sys.exit(1)

    if args.names and len(args.names) != len(args.folders):
        print(f"Error: Number of names ({len(args.names)}) must match number of folders ({len(args.folders)})")
        sys.exit(1)

    # Patch MANUAL_EXCLUSIVE_TESTS into plot_comparison before calling get_shared_test_set
    plot_comparison.MANUAL_EXCLUSIVE_TESTS = MANUAL_EXCLUSIVE_TESTS

    print(f"Comparing {len(args.folders)} folders with timeout={args.timeout}s")
    print(f"Using {len(MANUAL_EXCLUSIVE_TESTS)} exclusive large tests")
    print(f"Folders (in order): {', '.join(args.folders)}")

    # Compute metrics for each folder
    folder_metrics = OrderedDict()

    for i, folder_path in enumerate(args.folders):
        if args.names:
            folder_name = args.names[i]
        else:
            folder_name = Path(folder_path).name

        print(f"\nProcessing {folder_name} ({folder_path})...")
        metrics = compute_metrics_for_folder(folder_path, args.timeout, normalize_sataccel=args.normalize_sataccel)

        if not metrics['results']:
            print(f"  Warning: No valid results found in {folder_path}")
            continue

        folder_metrics[folder_name] = metrics
        excluded = len(metrics['results']) - metrics['total_count']
        timedout = metrics['total_count'] - metrics['solved_count']
        print(f"  Found {len(metrics['results'])} tests ({excluded} excluded, {timedout} timeout), "
              f"PAR-2: {metrics['par2_score']:.2f}s, Solved: {metrics['solved_count']}/{metrics['total_count']}")

    if len(folder_metrics) < 2:
        print("\nError: Need at least 2 folders with valid results")
        sys.exit(1)

    # Determine shared test set
    error_mode = "as timeout" if args.errors_as_timeout else "excluded"
    print(f"\nDetermining shared test set (errors={error_mode})...")
    shared_tests, exclusion_table = get_shared_test_set(folder_metrics, args.timeout, False, args.errors_as_timeout)

    if exclusion_table:
        print(f"\n=== Excluded Tests ({len(exclusion_table)} tests) ===")
        print(f"{'Excluding Folder':<30} {'Reason':<15} {'Test Case':<100}")
        print("-" * 105)
        for test_case, folder, reason in sorted(exclusion_table):
            print(f"{folder:<30} {reason:<15} {test_case:<100}")

    print(f"Shared test set: {len(shared_tests)} tests")

    if not shared_tests:
        print("Error: No shared tests found across all folders")
        sys.exit(1)

    # Compute metrics needed for speedup chart
    baseline_name = next(iter(folder_metrics.keys()))
    geomean_results = compute_geomean_speedups(folder_metrics, shared_tests, args.timeout, baseline_name, args.errors_as_timeout)

    # Print summary
    shared_par2_scores = compute_par2_on_shared_set(folder_metrics, shared_tests, args.timeout, args.errors_as_timeout)
    print(f"\n{'Folder':<30} {'PAR-2 (s)':<12} {'Solved/Total':<16} {'Geomean×':<12}")
    print("-" * 74)
    for folder_name in folder_metrics.keys():
        par2, solved, total = shared_par2_scores[folder_name]
        speed = geomean_results.get(folder_name, None)
        if folder_name == baseline_name:
            speed = 1.0
        speed_str = f"{speed:.4f}" if speed is not None else 'n/a'
        print(f"{folder_name:<30} {par2:<12.6f} {solved:>8}/{total:<8} {speed_str:<12}")

    # Generate per-test speedup chart only
    print(f"\nGenerating per-test speedup chart ({len(shared_tests)} tests)...")
    plot_per_test_speedups(folder_metrics, shared_tests, args.timeout, args.output_dir, baseline_name, geomean_results, large_fonts=args.large_fonts)

    # Dump CSV
    dump_comparison_csv(folder_metrics, shared_tests, args.timeout, args.output_dir, args.errors_as_timeout)


if __name__ == '__main__':
    main()
