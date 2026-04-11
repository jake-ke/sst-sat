#!/usr/bin/env python3
"""
Overall Performance Plot: PAR-2 and Geomean Speedup side-by-side in one figure.

Reuses parsing and metric computation from plot_comparison.py but renders
both charts as subplots (a) and (b) with large text by default.

Usage:
    python plot_overall_perf.py <folder1|file1> <folder2|file2> [...] [options]

Example:
    python plot_overall_perf.py ../data/base/seed3 results.txt ../data/minisat/ \\
        --names "Baseline" "SAT-Accel" "MiniSAT" --normalize-sataccel
"""

import sys
import argparse
from pathlib import Path
from collections import OrderedDict

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.backends.backend_pdf
from matplotlib.ticker import MaxNLocator, FormatStrFormatter

# Reuse all parsing / metric logic from plot_comparison
from plot_comparison import (
    compute_metrics_for_folder,
    get_shared_test_set,
    compute_par2_on_shared_set,
    compute_geomean_speedups,
    get_folder_colors,
    wrap_label,
)


def generate_figure(folder_metrics, folder_names, baseline_name, shared_tests,
                    timeout_seconds, errors_as_timeout, exclude_timeouts, pdf_path):
    """Generate a single (a)+(b) PDF for the given shared test set."""
    shared_par2 = compute_par2_on_shared_set(folder_metrics, shared_tests,
                                              timeout_seconds, errors_as_timeout)
    geomeans = compute_geomean_speedups(folder_metrics, shared_tests,
                                         timeout_seconds, baseline_name, errors_as_timeout)

    tag = "no-timeouts" if exclude_timeouts else "with-timeouts"
    print(f"\n[{tag}] {len(shared_tests)} shared tests")
    print(f"{'Folder':<30} {'PAR-2 (s)':<12} {'Solved/Total':<16} {'Geomean×':<12}")
    print("-" * 74)
    for fn in folder_names:
        par2, solved, total = shared_par2[fn]
        speed = geomeans.get(fn, None)
        if fn == baseline_name:
            speed = 1.0
        speed_str = f"{speed:.4f}" if speed is not None else 'n/a'
        print(f"{fn:<30} {par2:<12.6f} {solved:>8}/{total:<8} {speed_str:<12}")

    # --- Plot ---
    font_scale = 2.2
    fig_w = 14 * font_scale
    fig_h = 5.0 * font_scale

    par2_values = [shared_par2[n][0] for n in folder_names]
    solved_counts = [shared_par2[n][1] for n in folder_names]
    g_values = []
    for n in folder_names:
        s = geomeans.get(n, None)
        if n == baseline_name:
            s = 1.0
        g_values.append(s)
    plot_g = [v if v is not None else 0.0 for v in g_values]

    num = len(folder_names)
    folder_colors = get_folder_colors(folder_names)

    if num == 2:
        bar_width, bar_spacing = 0.5, 0.7
    elif num <= 4:
        bar_width, bar_spacing = 0.7, 1.0
    else:
        bar_width, bar_spacing = 1.4, 1.7
    x_positions = [i * bar_spacing for i in range(num)]

    with matplotlib.backends.backend_pdf.PdfPages(pdf_path) as pdf:
        fig, (ax_par2, ax_geo) = plt.subplots(1, 2, figsize=(fig_w, fig_h))

        # ---- (a) PAR-2 ----
        y_label = 'Runtime (s)' if exclude_timeouts else 'PAR-2 (s)'
        ax_par2.set_ylabel(y_label, fontsize=int(30 * font_scale))
        ax_par2.tick_params(axis='y', labelsize=int(26 * font_scale))
        ax_par2.grid(axis='y', alpha=0.3)

        bars_a = ax_par2.bar(x_positions, par2_values, width=bar_width,
                             color=folder_colors, alpha=0.85,
                             edgecolor='black', linewidth=0.8)
        total_count = shared_par2[folder_names[0]][2] if folder_names else 0
        for bar, par2, solved in zip(bars_a, par2_values, solved_counts):
            h = bar.get_height()
            timeout_count = total_count - solved
            if exclude_timeouts:
                bar_label = f'{par2:.2f} s'
            else:
                bar_label = f'{par2:.2f} s\nTO: {timeout_count}'
            ax_par2.text(bar.get_x() + bar.get_width() / 2., h,
                         bar_label,
                         ha='center', va='bottom', fontsize=int(20 * font_scale))
        from matplotlib.patches import Patch
        if exclude_timeouts:
            ax_par2.legend(handles=[Patch(facecolor='none', edgecolor='none',
                                         label=f'{total_count} tests')],
                           loc='upper right', fontsize=int(20 * font_scale),
                           frameon=True, handlelength=0, handletextpad=0)
        else:
            ax_par2.legend(handles=[Patch(facecolor='none', edgecolor='none', label='TO = Timeout')],
                           loc='upper right', fontsize=int(20 * font_scale),
                           frameon=True, handlelength=0, handletextpad=0)

        ax_par2.set_xticks(x_positions)
        ax_par2.set_xticklabels(folder_names,
                                fontsize=int(26 * font_scale),
                                rotation=30, ha='right', rotation_mode='anchor')
        for lbl in ax_par2.get_xticklabels():
            lbl.set_position((lbl.get_position()[0] + 0.2, lbl.get_position()[1]))
        max_par2 = max(par2_values) if par2_values else 1
        ax_par2.set_ylim(0, max_par2 * 1.30)

        # ---- (b) Geomean Speedup ----
        ax_geo.set_ylabel('Speedup', fontsize=int(30 * font_scale))
        ax_geo.tick_params(axis='y', labelsize=int(26 * font_scale))
        ax_geo.grid(axis='y', alpha=0.3)

        bars_b = ax_geo.bar(x_positions, plot_g, width=bar_width,
                            color=folder_colors, alpha=0.85,
                            edgecolor='black', linewidth=0.8)
        for bar, val in zip(bars_b, g_values):
            h = bar.get_height()
            label = 'n/a' if val is None else f'{val:.2f}\u00d7'
            ax_geo.text(bar.get_x() + bar.get_width() / 2., h,
                        label, ha='center', va='bottom',
                        fontsize=int(22 * font_scale))

        ax_geo.set_xticks(x_positions)
        ax_geo.set_xticklabels(folder_names,
                               fontsize=int(26 * font_scale),
                               rotation=30, ha='right', rotation_mode='anchor')
        for lbl in ax_geo.get_xticklabels():
            lbl.set_position((lbl.get_position()[0] + 0.2, lbl.get_position()[1]))
        ymax = max(plot_g) if plot_g else 1.0
        ax_geo.set_ylim(0, ymax * 1.30)
        ax_geo.yaxis.set_major_locator(MaxNLocator(nbins=5))
        ax_geo.yaxis.set_major_formatter(FormatStrFormatter('%.0f'))

        # Use set_xlabel for (a)/(b) — sits below rotated tick labels automatically
        ax_par2.set_xlabel('(a)', fontsize=int(28 * font_scale),
                           fontweight='bold', labelpad=12)
        ax_geo.set_xlabel('(b)', fontsize=int(28 * font_scale),
                          fontweight='bold', labelpad=12)

        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)

    print(f"Saved: {pdf_path}")


def main():
    parser = argparse.ArgumentParser(
        description='Plot PAR-2 and Geomean Speedup side-by-side in one figure (produces two PDFs)',
    )
    parser.add_argument('folders', nargs='+', help='Input folders or raw text files to compare')
    parser.add_argument('--names', nargs='+',
                        help='Custom display names (must match number of folders)')
    parser.add_argument('--timeout', type=float, default=36,
                        help='Timeout in seconds (default: 36)')
    parser.add_argument('--output-dir', default='results',
                        help='Output directory (default: results/)')
    parser.add_argument('--errors-as-timeout', action='store_true',
                        help='Treat ERROR/UNKNOWN as timeout instead of excluding')
    parser.add_argument('--normalize-sataccel', action='store_true',
                        help='Divide runtimes in .txt inputs by 4 (SatAccel clock normalization)')

    args = parser.parse_args()

    if len(args.folders) < 2:
        print("Error: Need at least 2 folders to compare")
        sys.exit(1)
    if args.names and len(args.names) != len(args.folders):
        print(f"Error: Number of names ({len(args.names)}) != folders ({len(args.folders)})")
        sys.exit(1)

    # --- Parse all folders ---
    folder_metrics = OrderedDict()
    for i, folder_path in enumerate(args.folders):
        folder_name = args.names[i] if args.names else Path(folder_path).name
        print(f"Processing {folder_name} ({folder_path})...")
        metrics = compute_metrics_for_folder(folder_path, args.timeout,
                                             normalize_sataccel=args.normalize_sataccel)
        if not metrics['results']:
            print(f"  Warning: No results in {folder_path}")
            continue
        folder_metrics[folder_name] = metrics

    if len(folder_metrics) < 2:
        print("Error: Need at least 2 folders with valid results")
        sys.exit(1)

    folder_names = list(folder_metrics.keys())
    baseline_name = next(iter(folder_metrics.keys()))
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # --- PDF 1: with timeouts (PAR-2) ---
    shared_tests, exclusion_table = get_shared_test_set(
        folder_metrics, args.timeout, False, args.errors_as_timeout)
    if exclusion_table:
        print(f"Excluded {len(exclusion_table)} tests (with-timeouts set)")
    if shared_tests:
        generate_figure(folder_metrics, folder_names, baseline_name, shared_tests,
                        args.timeout, args.errors_as_timeout, False,
                        output_dir / 'overall_perf.pdf')
    else:
        print("Error: No shared tests for with-timeouts set")

    # --- PDF 2: without timeouts (average runtime) ---
    shared_tests_no_to, exclusion_table_no_to = get_shared_test_set(
        folder_metrics, args.timeout, True, args.errors_as_timeout)
    if exclusion_table_no_to:
        print(f"\nExcluded {len(exclusion_table_no_to)} tests (no-timeouts set)")
    if shared_tests_no_to:
        generate_figure(folder_metrics, folder_names, baseline_name, shared_tests_no_to,
                        args.timeout, args.errors_as_timeout, True,
                        output_dir / 'overall_perf_no_timeouts.pdf')
    else:
        print("Error: No shared tests for no-timeouts set")


if __name__ == '__main__':
    main()
