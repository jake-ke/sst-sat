#!/usr/bin/env python3
"""
SAT Solver Pie Chart Breakdown Plotter

Plots 3 side-by-side pie charts or a nested donut chart showing runtime
breakdown for Baseline, +Prop, and SATBlast configurations.

Usage: python plot_pie_breakdown.py <baseline_folder> <output.pdf> \
           --folder <folder> <name> --accel <accel_folder> [--donut]
"""

import argparse
from pathlib import Path
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
import numpy as np
from plot_breakdown import _parse_folder


# Plot order: Decision last so it's adjacent to Propagate (CCW from 3 o'clock)
COMPONENTS = ['Propagate', 'Analyze', 'Minimize',
              'Backtrack', 'Restart', 'Deletion', 'Priority Queue']
DISPLAY_NAMES = {
    'Priority Queue': 'Decision',
}
# Colors matching COMPONENTS plot order
COLORS = [
    '#4C72B0',  # Propagate (steel blue)
    '#DD8452',  # Analyze (warm orange)
    '#55A868',  # Minimize (sage green)
    '#C44E52',  # Backtrack (muted red)
    '#CCB974',  # Restart (gold)
    '#937860',  # Deletion (warm brown)
    '#8172B3',  # Decision (soft purple)
]
# Legend order (different from plot order)
LEGEND_ORDER = ['Propagate', 'Priority Queue', 'Analyze', 'Minimize',
                'Backtrack', 'Restart', 'Deletion']
LEGEND_COLORS = {
    'Propagate': '#4C72B0', 'Analyze': '#DD8452', 'Priority Queue': '#8172B3',
    'Minimize': '#55A868', 'Backtrack': '#C44E52', 'Restart': '#CCB974',
    'Deletion': '#937860',
}
PCT_THRESHOLD = 3.0  # only label slices above this %


def plot_pie_charts(datasets, output_pdf):
    """Plot side-by-side pie charts for each configuration."""
    valid = [(name, d) for name, d in datasets if d is not None]
    if not valid:
        print("Error: No valid data to plot")
        return

    n = len(valid)
    fig, axes = plt.subplots(1, n, figsize=(4.5 * n, 4.5))
    if n == 1:
        axes = [axes]
    fig.subplots_adjust(wspace=0.02)

    for ax, (name, data) in zip(axes, valid):
        bd = data['runtime_breakdown']
        sizes = [bd.get(c, 0) for c in COMPONENTS]

        def autopct_func(pct):
            return f'{pct:.0f}%' if pct >= PCT_THRESHOLD else ''

        wedges, texts, autotexts = ax.pie(
            sizes, autopct=autopct_func, startangle=90,
            colors=COLORS, pctdistance=0.65,
            textprops={'fontsize': 18},
        )
        for t in autotexts:
            t.set_fontsize(18)
            t.set_color('white')
            t.set_fontweight('bold')
        ax.set_title(name, fontsize=24, fontweight='bold', pad=-5)

    # Shared legend using LEGEND_ORDER (Decision 2nd)
    legend_patches = [mpatches.Patch(color=LEGEND_COLORS[c],
                      label=DISPLAY_NAMES.get(c, c))
                      for c in LEGEND_ORDER]
    fig.legend(handles=legend_patches, loc='lower center',
               ncol=len(LEGEND_ORDER), fontsize=18,
               handlelength=1.2, handletextpad=0.4,
               columnspacing=1.0, borderpad=0.2)

    plt.tight_layout(rect=[0, 0.06, 1, 0.98], w_pad=0.3)
    plt.savefig(output_pdf, format='pdf', bbox_inches='tight',
                pad_inches=0.05, dpi=300)
    print(f"Pie chart saved to: {output_pdf}")
    plt.close()


def plot_nested_donut(datasets, output_pdf):
    """Plot nested donut chart with concentric rings (inner=Baseline, outer=SATBlast)."""
    valid = [(name, d) for name, d in datasets if d is not None]
    if not valid:
        print("Error: No valid data to plot")
        return

    # Order: Baseline=innermost, SATBlast=outermost
    # valid is already [Baseline, +Prop, SATBlast] from datasets
    n = len(valid)
    fig, ax = plt.subplots(1, 1, figsize=(11, 14))
    ax.set_position([0.0, 0.25, 1.0, 0.78])

    ring_width = 0.30
    gap = 0.02
    outer_scale = 1.18
    radii = [(1.0 - (n - 1 - i) * (ring_width + gap)) * outer_scale for i in range(n)]

    # All rings start at 3 o'clock; Decision is last so it's adjacent to Propagate
    start = 0
    prop_idx = 0
    decision_idx = len(COMPONENTS) - 1  # Decision is last in plot order

    # Pre-compute mid-angles
    all_mid_angles = []
    for i in range(n):
        _, data = valid[i]
        bd = data['runtime_breakdown']
        sizes = [bd.get(c, 0) for c in COMPONENTS]
        total = sum(sizes)
        mid_angles = {}
        if total > 0:
            cum = float(start)
            for j, s in enumerate(sizes):
                sweep = s / total * 360
                mid_angles[j] = cum + sweep / 2
                cum += sweep
        all_mid_angles.append(mid_angles)

    # Shared angles for Propagate and Decision so labels align radially
    shared_angles = {}
    for comp_idx in [prop_idx, decision_idx]:
        angles = [all_mid_angles[i].get(comp_idx) for i in range(n)
                  if comp_idx in all_mid_angles[i]]
        if angles:
            shared_angles[comp_idx] = sum(angles) / len(angles)

    # Draw outer rings first so inner rings paint on top
    for i in reversed(range(n)):
        name, data = valid[i]
        bd = data['runtime_breakdown']
        sizes = [bd.get(c, 0) for c in COMPONENTS]
        outer_r = radii[i]
        inner_r = outer_r - ring_width

        wedges, texts, autotexts = ax.pie(
            sizes, radius=outer_r, autopct='',
            startangle=start, colors=COLORS,
            pctdistance=0.85,
            wedgeprops=dict(width=ring_width, edgecolor='white', linewidth=2),
            textprops={'fontsize': 1},
        )
        for t in texts:
            t.set_text('')

        # Add percentage labels for non-Propagate, non-Decision components
        total = sum(sizes)
        if total > 0:
            for j, s in enumerate(sizes):
                pct = s / total * 100
                if j == prop_idx or j == decision_idx:
                    continue  # handled separately at 3 o'clock
                if pct >= 3.0 and j in all_mid_angles[i]:
                    angle = all_mid_angles[i][j]
                    mid_rad = np.radians(angle)
                    label_r = inner_r + ring_width / 2
                    x = label_r * np.cos(mid_rad)
                    y = label_r * np.sin(mid_rad)
                    ax.text(x, y, f'{pct:.0f}%', ha='center', va='center',
                            fontsize=16, color='white', fontweight='bold')

    # Propagate % labels: horizontal at 3 o'clock (upper), aligned across rings
    # Decision % labels: horizontal at 3 o'clock (lower), aligned across rings
    prop_y_offset = ring_width * 0.42   # Propagate row y offset (above center)
    dec_y_offset = ring_width * 0.25    # Decision row y offset (below center, higher up)
    for i in range(n):
        bd = valid[i][1]['runtime_breakdown']
        outer_r = radii[i]
        inner_r = outer_r - ring_width
        label_r = inner_r + ring_width / 2
        sizes = [bd.get(c, 0) for c in COMPONENTS]
        total = sum(sizes)
        if total == 0:
            continue

        # Propagate % - at 3 o'clock, above center
        prop_pct = sizes[prop_idx] / total * 100
        ax.text(label_r, prop_y_offset, f'{prop_pct:.0f}%',
                ha='center', va='center',
                fontsize=16, color='white', fontweight='bold')

        # Decision % - at 3 o'clock, slightly below center
        dec_pct = sizes[decision_idx] / total * 100
        ax.text(label_r, -dec_y_offset, f'{dec_pct:.0f}%',
                ha='center', va='center',
                fontsize=16, color='white', fontweight='bold')

    # Config name labels: at 90° (top center, won't clip)
    name_angle_rad = np.radians(90)
    for i in range(n):
        config_name = valid[i][0]
        outer_r = radii[i]
        inner_r = outer_r - ring_width
        label_r = inner_r + ring_width / 2
        x = label_r * np.cos(name_angle_rad)
        y = label_r * np.sin(name_angle_rad)
        ax.text(x, y, config_name, ha='center', va='center',
                fontsize=24, color='white', fontweight='bold')

    ax.set_aspect('equal')

    # Legend uses original order (Decision 3rd), not plot order
    legend_patches = [mpatches.Patch(color=LEGEND_COLORS[c],
                      label=DISPLAY_NAMES.get(c, c))
                      for c in LEGEND_ORDER]
    ax.legend(handles=legend_patches, loc='lower center',
              fontsize=20, ncol=4,
              bbox_to_anchor=(0.5, -0.08),
              handlelength=1.4, handletextpad=0.5,
              columnspacing=1.0, borderpad=0.2)

    plt.tight_layout()
    plt.savefig(output_pdf, format='pdf', bbox_inches='tight',
                pad_inches=0.05, dpi=300)
    print(f"Nested donut chart saved to: {output_pdf}")
    plt.close()


def main():
    parser = argparse.ArgumentParser(
        description='SAT Solver Runtime Breakdown Pie Charts',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog='Example:\n'
               '  %(prog)s logs/baseline breakdown.pdf '
               '--folder logs/opt "+Prop" --accel logs/accel\n'
               '  %(prog)s logs/baseline donut.pdf '
               '--folder logs/opt "+Prop" --accel logs/accel --donut\n'
    )
    parser.add_argument('logs_folder', help='Path to baseline log folder')
    parser.add_argument('output_pdf', nargs='?', default='pie_breakdown.pdf',
                        help='Output PDF file path')
    parser.add_argument('--folder', nargs=2, metavar=('FOLDER', 'NAME'),
                        help='Middle config folder and display name')
    parser.add_argument('--accel', metavar='ACCEL_FOLDER',
                        help='Accelerator (SATBlast) logs folder')
    parser.add_argument('--donut', action='store_true',
                        help='Also generate nested donut chart (always generates both)')

    args = parser.parse_args()

    datasets = [('Baseline', _parse_folder(args.logs_folder, label='Baseline'))]

    if args.folder:
        folder_path, folder_name = args.folder
        datasets.append((folder_name, _parse_folder(folder_path, label=folder_name)))

    if args.accel:
        datasets.append(('SATBlast', _parse_folder(args.accel, label='SATBlast')))

    # Always generate both pie and donut charts
    pdf_path = Path(args.output_pdf)
    pie_pdf = str(pdf_path.parent / f"{pdf_path.stem}_pie{pdf_path.suffix}")
    donut_pdf = str(pdf_path.parent / f"{pdf_path.stem}_donut{pdf_path.suffix}")
    plot_pie_charts(datasets, pie_pdf)
    plot_nested_donut(datasets, donut_pdf)


if __name__ == '__main__':
    main()
