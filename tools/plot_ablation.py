#!/usr/bin/env python3
"""
Combined ablation study plot: (a) Lits sweep geomean speedup, (b) Multi-conflict learning geomean ratio.

Produces a single PDF with two side-by-side subplots aligned for IEEE figures.

Usage:
  python3 tools/plot_ablation.py --output-dir results/ablation/
"""

import sys
import math
import argparse
from pathlib import Path
from collections import OrderedDict
import matplotlib.pyplot as plt
import matplotlib.backends.backend_pdf
import matplotlib.ticker as mticker
from matplotlib.ticker import MaxNLocator, FormatStrFormatter

from plot_comparison import (
    compute_metrics_for_folder, get_shared_test_set, get_folder_colors,
    compute_geomean_speedups, compute_par2_on_shared_set,
)
from plot_learning_comparison import (
    compute_learning_geomean_ratios, get_learning_colors,
    LEARNING_METRICS, LEARNING_HATCHES,
)


def load_folders(folder_paths, names, timeout, errors_as_timeout=False):
    """Load metrics for a list of folders, return OrderedDict of folder_metrics."""
    folder_metrics = OrderedDict()
    for i, folder_path in enumerate(folder_paths):
        folder_name = names[i] if names else Path(folder_path).name
        print(f"  Processing {folder_name} ({folder_path})...")
        metrics = compute_metrics_for_folder(folder_path, timeout)
        if not metrics['results']:
            print(f"    Warning: No valid results in {folder_path}")
            continue
        folder_metrics[folder_name] = metrics
    return folder_metrics


def main():
    parser = argparse.ArgumentParser(description='Combined ablation study plot')
    parser.add_argument('--output-dir', default='results/ablation/',
                        help='Output directory for plots')
    parser.add_argument('--timeout', type=float, default=36,
                        help='Timeout in seconds (default: 36)')
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # ── (a) Lits ablation ──────────────────────────────────────────────
    lits_folders = [
        '../sat-isca26-data/lits3/lits-1/',
        '../sat-isca26-data/lits3/lits-4/',
        '~/sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3',
        '../sat-isca26-data/lits3/lits-16/',
    ]
    lits_names = ['1', '4', '8', '16']
    # Expand ~ in paths
    lits_folders = [str(Path(f).expanduser()) for f in lits_folders]

    print("Loading Lits ablation data...")
    lits_metrics = load_folders(lits_folders, lits_names, args.timeout)
    lits_shared, _ = get_shared_test_set(lits_metrics, args.timeout,
                                          errors_as_timeout=True)
    lits_baseline = next(iter(lits_metrics.keys()))
    lits_geomean = compute_geomean_speedups(lits_metrics, lits_shared,
                                             args.timeout, lits_baseline,
                                             errors_as_timeout=True)

    # ── (b) Multi-conflict learning ────────────────────────────────────
    confl_folders = [
        '../sat-isca26-data/confl3/confl1/',
        '../sat-isca26-data/confl3/confl4/',
        '~/sat-isca26-data/confl3/confl8/',
        '../sat-isca26-data/confl3/confl12/',
    ]
    confl_names = ['1', '4', '8', '12']
    confl_folders = [str(Path(f).expanduser()) for f in confl_folders]

    print("\nLoading multi-conflict learning data...")
    confl_metrics = load_folders(confl_folders, confl_names, args.timeout)
    confl_shared, _ = get_shared_test_set(confl_metrics, args.timeout)
    confl_baseline = next(iter(confl_metrics.keys()))
    confl_ratios = compute_learning_geomean_ratios(confl_metrics,
                                                    set(confl_shared),
                                                    confl_baseline)

    # ── Combined figure ────────────────────────────────────────────────
    fs = 58  # base font size in points

    # Left panel narrower (3 points), right panel wider (5 metric groups × 4 bars)
    fig, (ax_left, ax_right) = plt.subplots(
        1, 2, figsize=(36, 12),
        gridspec_kw={'width_ratios': [1, 2.2]},
        constrained_layout=True,
    )

    # ── Panel (a): Lits geomean speedup line ──────────────────────────
    folder_names_a = list(lits_metrics.keys())
    g_values = []
    for name in folder_names_a:
        speed = lits_geomean.get(name, None)
        if name == lits_baseline:
            speed = 1.0
        g_values.append(speed)
    plot_vals = [v if v is not None else 0.0 for v in g_values]

    num_a = len(folder_names_a)
    bar_spacing = 1.0
    x_pos_a = [i * bar_spacing for i in range(num_a)]

    ax_left.set_ylabel('Speedup', fontsize=fs + 6)
    ax_left.tick_params(axis='y', labelsize=fs - 2)
    ax_left.grid(axis='y', alpha=0.3)

    x_pad = bar_spacing * 0.5
    ax_left.set_xlim(x_pos_a[0] - x_pad, x_pos_a[-1] + x_pad)
    ax_left.fill_between(x_pos_a, plot_vals, alpha=0.12, color='#1f77b4', zorder=2)
    ax_left.plot(x_pos_a, plot_vals,
                 marker='o', markersize=18, linewidth=4,
                 color='#1f77b4', zorder=3,
                 markeredgecolor='white', markeredgewidth=2)

    for x, val in zip(x_pos_a, g_values):
        if val is None:
            label = 'n/a'
        elif val == 1.0:
            label = '1\u00d7'
        else:
            label = f'{val:.1f}\u00d7'
        y = val if val is not None else 0.0
        ax_left.annotate(label, (x, y), textcoords="offset points",
                         xytext=(0, 14),
                         ha='center', va='bottom',
                         fontsize=fs - 8, fontweight='bold')

    ax_left.set_xticks(x_pos_a)
    ax_left.set_xticklabels(folder_names_a, fontsize=fs + 4, ha='center')
    ax_left.tick_params(axis='x', pad=2)
    ax_left.set_xlabel('# Literal Workers', fontsize=fs + 6, labelpad=2)
    ymax_a = max(plot_vals) if plot_vals else 1.0
    ax_left.set_ylim(0, ymax_a * 1.3)
    ax_left.yaxis.set_major_locator(MaxNLocator(nbins=5))
    ax_left.yaxis.set_major_formatter(FormatStrFormatter('%.1f'))

    # (a) label - placed via fig.text below for alignment

    # ── Panel (b): Multi-conflict learning geomean ratio bars ─────────
    folder_names_b = list(confl_metrics.keys())
    folder_colors_b = get_learning_colors(folder_names_b)
    num_b = len(folder_names_b)
    num_groups = len(LEARNING_METRICS)

    bar_width = 0.7 / num_b
    x_base = list(range(num_groups))

    for fi, fname in enumerate(folder_names_b):
        x_positions = [x + fi * bar_width for x in x_base]
        values = []
        for key, _ in LEARNING_METRICS:
            v = confl_ratios[key].get(fname, None)
            values.append(v if v is not None else 0.0)
        ax_right.bar(x_positions, values, bar_width,
                     label=fname,
                     color=folder_colors_b[fi],
                     alpha=0.9, edgecolor='black', linewidth=0.5)

    ax_right.set_ylabel('Ratio', fontsize=fs + 6)
    group_centers = [x + bar_width * (num_b - 1) / 2 for x in x_base]
    ax_right.set_xticks(group_centers)
    ax_right.set_xticklabels([label for _, label in LEARNING_METRICS],
                             fontsize=fs + 4, ha='center', linespacing=0.8)
    ax_right.tick_params(axis='y', labelsize=fs - 2)
    ax_right.yaxis.set_major_formatter(mticker.FormatStrFormatter('%.1f'))
    ax_right.yaxis.set_major_locator(mticker.MaxNLocator(nbins=5))
    ax_right.grid(axis='y', alpha=0.3)
    ax_right.set_axisbelow(True)
    ax_right.axhline(y=1.0, color='darkred', linestyle='--', linewidth=1.5,
                     alpha=0.8, zorder=3)

    ax_right.set_ylim(0, 1.8)

    # "HIGHER/LOWER BETTER" annotations
    label_y = 1.8 * 0.8
    for gi, (key, _) in enumerate(LEARNING_METRICS):
        label_text = 'Higher\nBetter' if key == 'unit_learnt_clauses' else 'Lower\nBetter'
        ax_right.text(group_centers[gi], label_y, label_text,
                      ha='center', va='top',
                      fontsize=fs - 4, color='green',
                      fontweight='bold')

    # Add invisible dummy handle for "Conflicts" label in the legend row
    from matplotlib.patches import Patch
    dummy = Patch(facecolor='none', edgecolor='none', label='Conflicts:')
    handles, labels = ax_right.get_legend_handles_labels()
    handles.insert(0, dummy)
    labels.insert(0, 'Conflicts:')
    ax_right.legend(handles, labels, loc='upper center', fontsize=fs + 4,
                    frameon=True, ncol=num_b + 1,
                    handlelength=1.5, handletextpad=0.5, columnspacing=1.0,
                    borderpad=0.3, labelspacing=0.2)

    # ── Align panels and save ────────────────────────────────────────
    # (a) and (b) labels at same y position using fig.text
    # Get center x of each axis in figure coordinates
    left_center = (ax_left.get_position().x0 + ax_left.get_position().x1) / 2
    right_center = (ax_right.get_position().x0 + ax_right.get_position().x1) / 2
    label_y = -0.02  # just below the axes
    fig.text(left_center, label_y, '(a)', fontsize=fs + 20, fontweight='bold',
             ha='center', va='top')
    fig.text(right_center, label_y, '(b)', fontsize=fs + 20, fontweight='bold',
             ha='center', va='top')

    pdf_path = output_dir / 'ablation_combined.pdf'
    with matplotlib.backends.backend_pdf.PdfPages(pdf_path) as pdf:
        pdf.savefig(fig, bbox_inches='tight')
    plt.close(fig)
    print(f"\nCombined ablation plot saved to: {pdf_path}")


if __name__ == "__main__":
    main()
