#!/usr/bin/env python3
"""
SAT Solver Conflict Learning Statistics Comparison Plotter

Compares conflict learning statistics across multiple folders.
For each metric, computes the average across all shared test cases.
Generates:
  1. A grouped bar chart with all 4 metrics as groups, one bar per folder.
  2. A geomean ratio chart showing per-metric geometric mean ratio vs baseline.

Usage: python plot_learning_comparison.py <folder1> <folder2> [...] [--timeout SECONDS] [--output-dir DIR]
"""

import sys
import math
import argparse
from pathlib import Path
from collections import OrderedDict
import matplotlib.pyplot as plt
import matplotlib.backends.backend_pdf
import matplotlib.ticker as mticker

from plot_comparison import (
    compute_metrics_for_folder, get_shared_test_set, get_folder_colors,
)

FIG_SIZE = (13, 6)

LEARNING_METRICS = [
    ('avg_backtrack_level', 'BT\nLevel'),
    ('avg_lbd', 'LBD'),
    ('avg_learnt_clause_length', 'CL\nLength'),
    ('unit_learnt_clauses', 'Unit\nClauses'),
    ('decisions', 'Decisions'),
]

LEARNING_METRICS_NO_BT = [
    ('avg_lbd', 'LBD'),
    ('avg_learnt_clause_length', 'CL\nLength'),
    ('unit_learnt_clauses', 'Unit\nClauses'),
    ('decisions', 'Decisions'),
]


def get_learning_colors(folder_names):
    """Return a single-hue transitioning palette (light-to-dark blue)."""
    palette = [
        '#c6dbef',  # Lightest blue
        '#6baed6',  # Light blue
        '#2171b5',  # Medium blue
        '#08306b',  # Dark blue
        '#4292c6',  # Blue
        '#084594',  # Deep blue
    ]
    return palette[:len(folder_names)]


# Hatch patterns for B/W distinguishability
LEARNING_HATCHES = ['', '///', '...', 'xxx', '\\\\\\', '---']


def compute_learning_averages(folder_metrics, shared_tests):
    """Compute per-folder averages for each learning metric over shared tests.

    Returns: dict metric_key -> dict folder_name -> average_value
    """
    averages = {key: {} for key, _ in LEARNING_METRICS}

    for folder_name, metrics in folder_metrics.items():
        result_map = {}
        for r in metrics['results']:
            tc = r.get('test_case')
            if tc in shared_tests:
                result_map[tc] = r

        for key, _ in LEARNING_METRICS:
            vals = []
            for tc in shared_tests:
                r = result_map.get(tc)
                if r is None:
                    continue
                v = r.get(key)
                if v is not None:
                    try:
                        vals.append(float(v))
                    except (TypeError, ValueError):
                        pass
            averages[key][folder_name] = sum(vals) / len(vals) if vals else 0.0

    return averages


def compute_learning_geomean_ratios(folder_metrics, shared_tests, baseline_name):
    """Compute per-metric geometric mean ratio vs baseline across shared tests.

    For each metric and each non-baseline folder, computes:
        geomean(folder_value / baseline_value) across all shared tests.

    Returns: dict metric_key -> dict folder_name -> geomean_ratio
    """
    ratios = {key: {} for key, _ in LEARNING_METRICS}

    # Build per-folder result maps
    folder_maps = {}
    for folder_name, metrics in folder_metrics.items():
        rmap = {}
        for r in metrics['results']:
            tc = r.get('test_case')
            if tc in shared_tests:
                rmap[tc] = r
        folder_maps[folder_name] = rmap

    baseline_map = folder_maps.get(baseline_name, {})

    for key, _ in LEARNING_METRICS:
        for folder_name in folder_metrics.keys():
            if folder_name == baseline_name:
                ratios[key][folder_name] = 1.0
                continue

            ln_sum = 0.0
            n = 0
            fmap = folder_maps.get(folder_name, {})
            for tc in shared_tests:
                br = baseline_map.get(tc)
                fr = fmap.get(tc)
                if br is None or fr is None:
                    continue
                bv = br.get(key)
                fv = fr.get(key)
                if bv is None or fv is None:
                    continue
                try:
                    bv_f = float(bv)
                    fv_f = float(fv)
                except (TypeError, ValueError):
                    continue
                if bv_f <= 0 or fv_f <= 0:
                    continue
                ln_sum += math.log(fv_f / bv_f)
                n += 1
            ratios[key][folder_name] = math.exp(ln_sum / n) if n > 0 else None

    return ratios


def plot_learning_grouped(folder_metrics, shared_tests, output_dir, large_fonts=False):
    """Generate a single grouped bar chart: 4 metric groups, one bar per folder."""
    font_scale = 1.6 if large_fonts else 1.3
    fig_size = (FIG_SIZE[0] * (font_scale + 0.05), FIG_SIZE[1] * (font_scale + 0.05))

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    pdf_path = output_dir / 'learning_comparison.pdf'

    averages = compute_learning_averages(folder_metrics, shared_tests)
    folder_names = list(folder_metrics.keys())
    folder_colors = get_learning_colors(folder_names)
    num_folders = len(folder_names)
    num_groups = len(LEARNING_METRICS)

    bar_width = 0.7 / num_folders
    x_base = range(num_groups)

    with matplotlib.backends.backend_pdf.PdfPages(pdf_path) as pdf:
        fig, ax = plt.subplots(figsize=fig_size)

        for folder_idx, folder_name in enumerate(folder_names):
            x_positions = [x + folder_idx * bar_width for x in x_base]
            values = [averages[key].get(folder_name, 0.0) for key, _ in LEARNING_METRICS]
            ax.bar(x_positions, values, bar_width,
                   label=folder_name,
                   color=folder_colors[folder_idx],
                   hatch=LEARNING_HATCHES[folder_idx % len(LEARNING_HATCHES)],
                   alpha=0.9, edgecolor='black', linewidth=0.5)

            # Value annotations
            for x, val in zip(x_positions, values):
                ax.text(x, val * 1.02, f'{val:.1f}', ha='center', va='bottom',
                        fontsize=int(24 * font_scale))

        ax.set_ylabel('Value', fontsize=int(32 * font_scale))
        ax.set_xticks([x + bar_width * (num_folders - 1) / 2 for x in x_base])
        ax.set_xticklabels([label for _, label in LEARNING_METRICS],
                           fontsize=int(30 * font_scale), ha='center')
        ax.tick_params(axis='y', labelsize=int(28 * font_scale))
        ax.yaxis.set_major_formatter(mticker.FormatStrFormatter('%.1f'))
        ax.grid(axis='y', alpha=0.3)
        ax.set_axisbelow(True)

        all_vals = [averages[key].get(name, 0.0) for key, _ in LEARNING_METRICS for name in folder_names]
        max_val = max(all_vals) if all_vals else 1
        ax.set_ylim(0, max_val * 1.3)

        ax.legend(loc='upper center', fontsize=int(26 * font_scale), frameon=False,
                  ncol=min(num_folders, 5), bbox_to_anchor=(0.5, 1.02),
                  handlelength=1.0, handletextpad=0.5, columnspacing=1.0)

        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)

    print(f"Learning comparison chart saved to: {pdf_path}")


def plot_learning_per_test_ratios(folder_metrics, shared_tests, output_dir, baseline_name, large_fonts=False):
    """Generate per-test bar charts for each learning metric, all in one PDF.

    For each metric, plots grouped bars per test with one bar per folder
    (absolute values). Sorted by baseline value. One page per metric, 4 pages total.
    """
    font_scale = 1.6 if large_fonts else 1.3
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    pdf_path = output_dir / 'learning_per_test.pdf'

    folder_names = list(folder_metrics.keys())
    folder_colors = get_learning_colors(folder_names)
    num_folders = len(folder_names)

    # Build per-folder result maps
    folder_maps = {}
    for folder_name, metrics in folder_metrics.items():
        rmap = {}
        for r in metrics['results']:
            tc = r.get('test_case')
            if tc in shared_tests:
                rmap[tc] = r
        folder_maps[folder_name] = rmap

    baseline_map = folder_maps.get(baseline_name, {})

    with matplotlib.backends.backend_pdf.PdfPages(pdf_path) as pdf:
        for key, label in LEARNING_METRICS:
            # Get tests that have valid values in all folders
            valid_tests = []
            for tc in shared_tests:
                all_valid = True
                for folder_name in folder_names:
                    r = folder_maps.get(folder_name, {}).get(tc)
                    if r is None or r.get(key) is None:
                        all_valid = False
                        break
                if all_valid:
                    valid_tests.append(tc)

            if not valid_tests:
                continue

            # Sort by baseline value (ascending)
            valid_tests.sort(key=lambda tc: float(baseline_map[tc].get(key, 0)))

            labels = [tc[:6] for tc in valid_tests]
            fig_width = max(15, len(valid_tests) * 0.6)
            fig, ax = plt.subplots(figsize=(fig_width, 6))

            bar_width = 0.8 / num_folders
            x_base = range(len(valid_tests))

            for folder_idx, folder_name in enumerate(folder_names):
                x_positions = [x + folder_idx * bar_width for x in x_base]
                values = [float(folder_maps[folder_name][tc].get(key, 0))
                          for tc in valid_tests]
                ax.bar(x_positions, values, bar_width,
                       label=folder_name,
                       color=folder_colors[folder_idx],
                       hatch=LEARNING_HATCHES[folder_idx % len(LEARNING_HATCHES)],
                       alpha=0.85, edgecolor='black', linewidth=0.5)

            ax.set_ylabel(label, fontsize=int(18 * font_scale))
            ax.set_xticks([x + bar_width * (num_folders - 1) / 2 for x in x_base])
            ax.set_xticklabels(labels, fontsize=max(7, int(10 * font_scale)),
                               rotation=45, ha='right')
            ax.tick_params(axis='y', labelsize=int(14 * font_scale))
            ax.yaxis.set_major_formatter(mticker.FormatStrFormatter('%.1f'))
            ax.grid(axis='y', alpha=0.3, linestyle='--')
            ax.set_axisbelow(True)

            ax.legend(loc='upper center', fontsize=int(22 * font_scale), frameon=True,
                      ncol=min(num_folders, 5), bbox_to_anchor=(0.5, 1.02),
                      handlelength=1.0, handletextpad=0.5, columnspacing=1.0)

            plt.tight_layout()
            pdf.savefig(fig, bbox_inches='tight')
            plt.close(fig)

    print(f"Learning per-test charts saved to: {pdf_path}")


def plot_learning_geomean(folder_metrics, shared_tests, output_dir, baseline_name, large_fonts=False):
    """Generate a grouped bar chart of per-metric geomean ratios vs baseline."""
    font_scale = 2.0 if large_fonts else 1.5
    fig_size = (FIG_SIZE[0] * (font_scale + 0.05), FIG_SIZE[1] * (font_scale + 0.05))

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    pdf_path = output_dir / 'learning_geomean_ratio.pdf'

    ratios = compute_learning_geomean_ratios(folder_metrics, shared_tests, baseline_name)
    folder_names = list(folder_metrics.keys())
    folder_colors = get_learning_colors(folder_names)
    num_folders = len(folder_names)
    num_groups = len(LEARNING_METRICS)

    bar_width = 0.7 / num_folders
    x_base = range(num_groups)

    with matplotlib.backends.backend_pdf.PdfPages(pdf_path) as pdf:
        fig, ax = plt.subplots(figsize=fig_size)

        for folder_idx, folder_name in enumerate(folder_names):
            x_positions = [x + folder_idx * bar_width for x in x_base]
            values = []
            for key, _ in LEARNING_METRICS:
                v = ratios[key].get(folder_name, None)
                values.append(v if v is not None else 0.0)
            ax.bar(x_positions, values, bar_width,
                   label=folder_name,
                   color=folder_colors[folder_idx],
                   hatch=LEARNING_HATCHES[folder_idx % len(LEARNING_HATCHES)],
                   alpha=0.9, edgecolor='black', linewidth=0.5)

        ax.set_ylabel('Geomean Ratio', fontsize=int(32 * font_scale))
        group_centers = [x + bar_width * (num_folders - 1) / 2 for x in x_base]
        ax.set_xticks(group_centers)
        ax.set_xticklabels([label for _, label in LEARNING_METRICS],
                           fontsize=int(30 * font_scale), ha='center')
        ax.tick_params(axis='y', labelsize=int(28 * font_scale))
        ax.yaxis.set_major_formatter(mticker.FormatStrFormatter('%.1f'))
        ax.yaxis.set_major_locator(mticker.MaxNLocator(nbins=5))
        ax.grid(axis='y', alpha=0.3)
        ax.set_axisbelow(True)
        ax.axhline(y=1.0, color='darkred', linestyle='--', linewidth=1.5, alpha=0.8, zorder=3)

        all_vals = [ratios[key].get(name, 0.0) or 0.0 for key, _ in LEARNING_METRICS for name in folder_names]
        max_val = max(all_vals) if all_vals else 1.0
        ax.set_ylim(0, max(max_val * 1.3, 1.5))

        # Add "Lower Better" / "Higher Better" labels per group above the 1.0 line
        label_y = max_val * 1.3 * 0.90  # above bars, below legend
        for gi, (key, _) in enumerate(LEARNING_METRICS):
            label_text = 'Higher\nBetter' if key == 'unit_learnt_clauses' else 'Lower\nBetter'
            ax.text(group_centers[gi], label_y, label_text, ha='center', va='top',
                    fontsize=int(22 * font_scale), color='green', fontweight='bold')

        ax.legend(loc='upper center', fontsize=int(30 * font_scale), frameon=False,
                  ncol=min(num_folders, 5), bbox_to_anchor=(0.5, 1.02),
                  handlelength=1.0, handletextpad=0.5, columnspacing=1.0)

        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)

    print(f"Learning geomean ratio chart saved to: {pdf_path}")


def plot_learning_geomean_no_bt(folder_metrics, shared_tests, output_dir, baseline_name, large_fonts=False):
    """Generate a grouped bar chart of per-metric geomean ratios vs baseline, excluding Avg BT Level."""
    font_scale = 2.0 if large_fonts else 1.5
    fig_size = (FIG_SIZE[0] * (font_scale + 0.05), FIG_SIZE[1] * (font_scale + 0.05))

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    pdf_path = output_dir / 'learning_geomean_ratio_no_bt.pdf'

    # Compute ratios using full LEARNING_METRICS, then filter
    ratios = compute_learning_geomean_ratios(folder_metrics, shared_tests, baseline_name)
    folder_names = list(folder_metrics.keys())
    folder_colors = get_learning_colors(folder_names)
    num_folders = len(folder_names)
    metrics = LEARNING_METRICS_NO_BT
    num_groups = len(metrics)

    bar_width = 0.7 / num_folders
    x_base = range(num_groups)

    with matplotlib.backends.backend_pdf.PdfPages(pdf_path) as pdf:
        fig, ax = plt.subplots(figsize=fig_size)

        for folder_idx, folder_name in enumerate(folder_names):
            x_positions = [x + folder_idx * bar_width for x in x_base]
            values = []
            for key, _ in metrics:
                v = ratios[key].get(folder_name, None)
                values.append(v if v is not None else 0.0)
            ax.bar(x_positions, values, bar_width,
                   label=folder_name,
                   hatch=LEARNING_HATCHES[folder_idx % len(LEARNING_HATCHES)],
                   color=folder_colors[folder_idx],
                   alpha=0.9, edgecolor='black', linewidth=0.5)

        ax.set_ylabel('Geomean Ratio', fontsize=int(32 * font_scale))
        group_centers = [x + bar_width * (num_folders - 1) / 2 for x in x_base]
        ax.set_xticks(group_centers)
        ax.set_xticklabels([label for _, label in metrics],
                           fontsize=int(30 * font_scale), ha='center')
        ax.tick_params(axis='y', labelsize=int(28 * font_scale))
        ax.yaxis.set_major_formatter(mticker.FormatStrFormatter('%.1f'))
        ax.yaxis.set_major_locator(mticker.MaxNLocator(nbins=5))
        ax.grid(axis='y', alpha=0.3)
        ax.set_axisbelow(True)
        ax.axhline(y=1.0, color='darkred', linestyle='--', linewidth=1.5, alpha=0.8, zorder=3)

        all_vals = [ratios[key].get(name, 0.0) or 0.0 for key, _ in metrics for name in folder_names]
        max_val = max(all_vals) if all_vals else 1.0
        ax.set_ylim(0, max(max_val * 1.3, 1.5))

        # Add "Lower Better" / "Higher Better" labels per group above the 1.0 line
        label_y = max_val * 1.3 * 0.90  # above bars, below legend
        for gi, (key, _) in enumerate(metrics):
            label_text = 'Higher\nBetter' if key == 'unit_learnt_clauses' else 'Lower\nBetter'
            ax.text(group_centers[gi], label_y, label_text, ha='center', va='top',
                    fontsize=int(22 * font_scale), color='green', fontweight='bold')

        ax.legend(loc='upper center', fontsize=int(30 * font_scale), frameon=False,
                  ncol=min(num_folders, 5), bbox_to_anchor=(0.5, 1.02),
                  handlelength=1.0, handletextpad=0.5, columnspacing=1.0)

        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)

    print(f"Learning geomean ratio chart (no BT level) saved to: {pdf_path}")


def main():
    parser = argparse.ArgumentParser(
        description='Compare conflict learning statistics across folders',
    )
    parser.add_argument('folders', nargs='+', help='Input folders to compare')
    parser.add_argument('--names', nargs='+',
                        help='Custom names for each folder')
    parser.add_argument('--timeout', type=float, default=36,
                        help='Timeout in seconds (default: 36)')
    parser.add_argument('--output-dir', default='results',
                        help='Output directory for plots (default: results/)')
    parser.add_argument('--large-fonts', action='store_true',
                        help='Use larger font sizes')

    args = parser.parse_args()

    if len(args.folders) < 2:
        print("Error: Need at least 2 folders to compare")
        sys.exit(1)

    if args.names and len(args.names) != len(args.folders):
        print(f"Error: Number of names ({len(args.names)}) must match number of folders ({len(args.folders)})")
        sys.exit(1)

    print(f"Comparing {len(args.folders)} folders with timeout={args.timeout}s")

    folder_metrics = OrderedDict()
    used_names = set()
    for i, folder_path in enumerate(args.folders):
        if args.names:
            folder_name = args.names[i]
        else:
            folder_name = Path(folder_path).name
            if folder_name in used_names:
                folder_name = f"{Path(folder_path).parent.name}/{folder_name}"
            used_names.add(folder_name)
        print(f"\nProcessing {folder_name} ({folder_path})...")
        metrics = compute_metrics_for_folder(folder_path, args.timeout)

        if not metrics['results']:
            print(f"  Warning: No valid results found in {folder_path}")
            continue

        folder_metrics[folder_name] = metrics
        print(f"  Found {len(metrics['results'])} tests")

    if len(folder_metrics) < 2:
        print("\nError: Need at least 2 folders with valid results")
        sys.exit(1)

    shared_tests, exclusion_table = get_shared_test_set(folder_metrics, args.timeout)

    if exclusion_table:
        print(f"\nExcluded {len(exclusion_table)} tests from shared set")

    print(f"Shared test set: {len(shared_tests)} tests")

    if not shared_tests:
        print("Error: No shared tests found across all folders")
        sys.exit(1)

    shared_set = set(shared_tests)
    baseline_name = next(iter(folder_metrics.keys()))

    # Print averages table
    averages = compute_learning_averages(folder_metrics, shared_set)
    header = f"{'Folder':<30}"
    for _, label in LEARNING_METRICS:
        header += f" {label:<14}"
    print(f"\n{header}")
    print("-" * (30 + 15 * len(LEARNING_METRICS)))
    for folder_name in folder_metrics.keys():
        row = f"{folder_name:<30}"
        for key, _ in LEARNING_METRICS:
            v = averages[key].get(folder_name, 0.0)
            row += f" {v:<14.2f}"
        print(row)

    # Print geomean ratios table
    ratios = compute_learning_geomean_ratios(folder_metrics, shared_set, baseline_name)
    print(f"\nGeomean Ratio vs {baseline_name}:")
    print(f"{header}")
    print("-" * (30 + 15 * len(LEARNING_METRICS)))
    for folder_name in folder_metrics.keys():
        row = f"{folder_name:<30}"
        for key, _ in LEARNING_METRICS:
            v = ratios[key].get(folder_name, None)
            row += f" {(f'{v:.4f}x' if v is not None else 'n/a'):<14}"
        print(row)

    # Generate plots (1 PDF each)
    plot_learning_grouped(folder_metrics, shared_set, args.output_dir, args.large_fonts)
    plot_learning_geomean(folder_metrics, shared_set, args.output_dir, baseline_name, args.large_fonts)
    plot_learning_geomean_no_bt(folder_metrics, shared_set, args.output_dir, baseline_name, args.large_fonts)
    plot_learning_per_test_ratios(folder_metrics, shared_set, args.output_dir, baseline_name, args.large_fonts)


if __name__ == "__main__":
    main()
