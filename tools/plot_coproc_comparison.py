#!/usr/bin/env python3
"""
Coproc vs SATBlast Comparison Plotter

Generates a single PDF with two side-by-side plots:
  Left:  Geomean speedup of SATBlast vs Coproc (Luby restart only)
  Right: Per-test runtime grouped bars for two hardcoded tests across all 4 configs

For Coproc folders, runtimes are scaled using compute_coprocessor() cycle estimates
rather than raw sim_time_ms.

Usage:
  python plot_coproc_comparison.py <satblast-luby> <satblast-lbd> <coproc-luby> <coproc-lbd> \
    [--timeout 36] [--output-dir results/] [--exclude-timeouts-geomean]
"""

import sys
import argparse
from pathlib import Path
from collections import OrderedDict
import matplotlib.pyplot as plt
import matplotlib.backends.backend_pdf

from plot_comparison import (
    compute_metrics_for_folder,
    normalize_test_case,
    get_shared_test_set,
    compute_geomean_speedups,
)
from compute_coprocessor import compute_coprocessor
from unified_parser import parse_satsolver_log

# Two hardcoded tests for the right plot
HARDCODED_TESTS = [
    "483e67e61c27723eec3cfa3dbe4f07c3-mrpp_4x4#8_8.cnf",
    "431bfb466be51ecd86b321b5cbce6d3c-mrpp_8x8#22_10.cnf",
]

# Config names in display order (positional arg order)
CONFIG_NAMES = [
    "SATBlast (Luby)",
    "Coproc (Luby)",
    "Coproc (LBD)",
]

# Which configs need coprocessor scaling
COPROC_CONFIGS = {"Coproc (Luby)", "Coproc (LBD)"}

# Colorblind-friendly palette — no red or orange
COLORS = {
    "SATBlast (Luby)": "#1f77b4",  # blue
    "Coproc (Luby)":   "#6a51a3",  # medium purple
    "Coproc (LBD)":    "#b5a8d4",  # light lavender
}


def apply_coproc_scaling(folder_path, metrics, timeout_seconds, roundtrip=10, sf_scale=1.0):
    """Scale sim_time_ms in metrics results using compute_coprocessor() cycle estimates.

    For each test, reads the original log, computes coproc/hw cycle ratio, and
    replaces sim_time_ms with scaled value: sim_time_ms * (coproc_cycles / hw_cycles).

    Modifies metrics in place.
    """
    folder = Path(folder_path)
    timeout_ms = timeout_seconds * 1000.0
    par2_penalty_ms = 2 * timeout_ms

    # Build map: normalized test_case -> log file path(s)
    seed_dirs = sorted(folder.glob('seed*'))
    if not seed_dirs:
        seed_dirs = [folder]

    # For multi-seed: compute ratio per seed per test, then average the scaled times
    # Build: test_case -> list of (sim_time_ms, coproc_ratio)
    test_seed_data = {}
    for sd in seed_dirs:
        for log_file in sorted(sd.glob('*.log')):
            if log_file.name == 'summary.log':
                continue
            content = log_file.read_text()
            result = parse_satsolver_log(str(log_file), content)
            tc_raw = result.get('test_case', '')
            tc_norm = normalize_test_case(tc_raw)

            sim_ms = float(result.get('sim_time_ms', 0.0) or 0.0)
            if sim_ms <= 0:
                continue

            has_coproc = any(k.startswith('coproc_') for k in result)
            if not has_coproc:
                # No coproc stats — use hw time as-is (ratio = 1.0)
                ratio = 1.0
            else:
                coproc, hw = compute_coprocessor(result, roundtrip, sf_scale)
                hw_total = sum(hw.values())
                coproc_total = sum(coproc.values())
                ratio = coproc_total / hw_total if hw_total > 0 else 1.0

            scaled_ms = sim_ms * ratio
            test_seed_data.setdefault(tc_norm, []).append(scaled_ms)

    # Average scaled times across seeds
    avg_scaled = {}
    for tc, times in test_seed_data.items():
        avg_scaled[tc] = sum(times) / len(times)

    # Apply to metrics results
    for r in metrics['results']:
        tc = r.get('test_case', '')
        if tc in avg_scaled:
            result_label = r.get('result', '')
            primary = result_label.split()[0] if result_label else ''
            if primary in ('SAT', 'UNSAT'):
                r['sim_time_ms'] = avg_scaled[tc]
                # Re-check timeout
                if r['sim_time_ms'] > timeout_ms:
                    r['result'] = 'TIMEOUT'
                    r['sim_time_ms'] = par2_penalty_ms
            elif primary == 'TIMEOUT':
                r['sim_time_ms'] = par2_penalty_ms


def extract_test_runtime(metrics, test_name):
    """Extract sim_time_ms for a specific test from folder metrics.
    Returns the runtime in ms, or None if not found.
    """
    norm_target = normalize_test_case(test_name)
    for r in metrics['results']:
        if r.get('test_case') == norm_target or normalize_test_case(r.get('test_case', '')) == norm_target:
            try:
                return float(r.get('sim_time_ms', 0.0) or 0.0)
            except (TypeError, ValueError):
                return None
    return None


def main():
    parser = argparse.ArgumentParser(
        description='Compare SATBlast vs Coproc across Luby and LBD restart strategies')
    parser.add_argument('folders', nargs=3,
                        help='3 folders: SATBlast-Luby, Coproc-Luby, Coproc-LBD')
    parser.add_argument('--timeout', type=float, default=36,
                        help='Timeout in seconds (default: 36)')
    parser.add_argument('--output-dir', default='results/',
                        help='Output directory (default: results/)')
    parser.add_argument('--exclude-timeouts-geomean', action='store_true',
                        help='Exclude TIMEOUT tests from geomean calculation')
    parser.add_argument('--roundtrip', type=int, default=10,
                        help='Coproc round-trip latency in cycles (default: 10)')
    parser.add_argument('--sf-scale', type=float, default=1.0,
                        help='Coproc serialization factor scale (default: 1.0)')

    args = parser.parse_args()

    # Parse all 3 folders
    all_metrics = OrderedDict()
    for i, folder_path in enumerate(args.folders):
        name = CONFIG_NAMES[i]
        print(f"Processing {name} ({folder_path})...")
        metrics = compute_metrics_for_folder(folder_path, args.timeout)
        if not metrics['results']:
            print(f"  Warning: No valid results in {folder_path}")
            sys.exit(1)

        # Apply coproc scaling for coproc configs
        if name in COPROC_CONFIGS:
            print(f"  Applying coproc scaling (roundtrip={args.roundtrip}, sf_scale={args.sf_scale})...")
            apply_coproc_scaling(folder_path, metrics, args.timeout, args.roundtrip, args.sf_scale)

        all_metrics[name] = metrics
        print(f"  Found {len(metrics['results'])} tests, Solved: {metrics['solved_count']}/{metrics['total_count']}")

    # --- Left plot data: per-test geomean slowdown vs SATBlast (Luby) ---
    import math
    shared_all, _ = get_shared_test_set(all_metrics, args.timeout, args.exclude_timeouts_geomean)
    print(f"\nShared tests (all 3 configs): {len(shared_all)}")

    timeout_ms = args.timeout * 1000.0
    penalty_ms = 2 * timeout_ms

    # Build test_case -> sim_time_ms lookup per config
    config_time_maps = {}
    for name, metrics in all_metrics.items():
        tmap = {}
        for r in metrics['results']:
            tc = r.get('test_case', '')
            res = r.get('result', '')
            sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
            primary = res.split()[0] if res else ''
            if primary in ('SAT', 'UNSAT') and sim_ms <= timeout_ms:
                tmap[tc] = sim_ms
            else:
                tmap[tc] = penalty_ms
        config_time_maps[name] = tmap

    # Compute geomean slowdown for Coproc configs vs SATBlast (Luby) baseline
    baseline = "SATBlast (Luby)"
    geomean_slowdowns = {baseline: 1.0}
    for name in ["Coproc (Luby)", "Coproc (LBD)"]:
        ln_sum = 0.0
        n = 0
        for tc in shared_all:
            t_base = config_time_maps[baseline].get(tc)
            t_cfg = config_time_maps[name].get(tc)
            if t_base and t_cfg and t_base > 0 and t_cfg > 0:
                ln_sum += math.log(t_cfg / t_base)
                n += 1
        geomean_slowdowns[name] = math.exp(ln_sum / n) if n > 0 else 1.0
        print(f"  {name} geomean slowdown: {geomean_slowdowns[name]:.4f}x ({n} tests)")

    # --- Right plot data: per-test runtimes ---
    right_configs = ["SATBlast (Luby)", "Coproc (Luby)", "Coproc (LBD)"]
    runtimes = {}  # {test_name: {config_name: ms}}
    for test in HARDCODED_TESTS:
        runtimes[test] = {}
        for cfg in right_configs:
            ms = extract_test_runtime(all_metrics[cfg], test)
            if ms is None:
                print(f"  Warning: {test} not found in {cfg}")
                ms = 0.0
            runtimes[test][cfg] = ms
            print(f"  {cfg} / {test[:8]}...: {ms:.1f} ms")

    # --- Plotting helper ---
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    def make_plot(pdf_name, left_ylabel, left_labels, left_values, left_colors):
        fs = 52
        plt.rcParams.update({'font.size': fs})
        fig, (ax_left, ax_right) = plt.subplots(
            1, 2, figsize=(36, 8),
            gridspec_kw={'width_ratios': [2.5, 3], 'wspace': 0.35}
        )

        # Left plot
        bars = ax_left.bar(left_labels, left_values, width=0.5, color=left_colors,
                           alpha=0.85, edgecolor='black', linewidth=0.8)
        for bar, val in zip(bars, left_values):
            if val == 1.0 and left_ylabel == 'Geomean Slowdown':
                continue  # skip 1.00x baseline label
            label = f'{val:.1f}x' if left_ylabel == 'Geomean Slowdown' else f'{val:.0f}'
            ax_left.text(bar.get_x() + bar.get_width() / 2., bar.get_height(),
                         label, ha='center', va='bottom', fontsize=fs - 12)
        ax_left.set_xlim(-0.6, len(left_labels) - 0.4)
        ax_left.set_ylabel("Slowdown")
        ax_left.set_ylim(0, max(left_values) * 1.3)
        if left_ylabel == 'Geomean Slowdown':
            ax_left.axhline(y=1.0, color='gray', linestyle='--', linewidth=0.8, alpha=0.6)
        ax_left.grid(axis='y', alpha=0.3)

        # Right plot: grouped bar chart
        num_configs = len(right_configs)
        bar_width = 0.8 / num_configs
        test_labels = [t[:8] for t in HARDCODED_TESTS]
        x_base = range(len(HARDCODED_TESTS))

        for ci, cfg in enumerate(right_configs):
            x_positions = [x + ci * bar_width for x in x_base]
            values = [runtimes[t][cfg] for t in HARDCODED_TESTS]
            ax_right.bar(x_positions, values, bar_width, label=cfg,
                         color=COLORS[cfg], alpha=0.85, edgecolor='black', linewidth=0.5)
            if cfg != "SATBlast (Luby)":
                baseline_values = [runtimes[t]["SATBlast (Luby)"] for t in HARDCODED_TESTS]
                for x, v, bv in zip(x_positions, values, baseline_values):
                    if bv > 0:
                        sd = v / bv
                        ax_right.text(x, v, f'{sd:.1f}x', ha='center', va='bottom',
                                      fontsize=fs - 12)

        ax_right.set_ylabel('Runtime (ms)')
        ax_right.set_xticks([x + bar_width * (num_configs - 1) / 2 for x in x_base])
        ax_right.set_xticklabels(test_labels)
        all_vals = [runtimes[t][cfg] for t in HARDCODED_TESTS for cfg in right_configs]
        ax_right.set_ylim(0, max(all_vals) * 1.25)
        ax_right.grid(axis='y', alpha=0.3)

        # Legend: horizontal, center top, above the plots
        handles, labels = ax_right.get_legend_handles_labels()
        fig.legend(handles, labels, frameon=True,
                   loc='upper center', bbox_to_anchor=(0.5, 1.12),
                   ncol=len(right_configs), fontsize=fs - 4,
                   handletextpad=0.3, columnspacing=1.0)

        fig.tight_layout(rect=[0, 0, 1, 0.85])

        # (a) and (b) labels: find the lowest tick label bottom across both axes,
        # then place both labels at the same y below that.
        fig.canvas.draw()
        min_y = 1.0
        for ax in [ax_left, ax_right]:
            for label in ax.get_xticklabels():
                bb = label.get_window_extent(renderer=fig.canvas.get_renderer())
                bb_fig = bb.transformed(fig.transFigure.inverted())
                min_y = min(min_y, bb_fig.y0)
        label_y = min_y - 0.12  # padding below lowest tick label
        left_center = (ax_left.get_position().x0 + ax_left.get_position().x1) / 2
        right_center = (ax_right.get_position().x0 + ax_right.get_position().x1) / 2
        fig.text(left_center, label_y, '(a)', ha='center', fontsize=fs + 10, fontweight='bold')
        fig.text(right_center, label_y, '(b)', ha='center', fontsize=fs + 10, fontweight='bold')

        pdf_path = output_dir / pdf_name
        with matplotlib.backends.backend_pdf.PdfPages(pdf_path) as pdf:
            pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)
        print(f"Saved to: {pdf_path}")

    # --- Plot 1: with Coproc (LBD) in (a) ---
    make_plot(
        'coproc_geomean_lbd.pdf',
        'Geomean Slowdown',
        ["SATBlast", "Coproc\n(Luby)", "Coproc\n(LBD)"],
        [geomean_slowdowns[c] for c in CONFIG_NAMES],
        [COLORS[c] for c in CONFIG_NAMES],
    )

    # --- Plot 2: without Coproc (LBD) in (a) ---
    luby_only = ["SATBlast (Luby)", "Coproc (Luby)"]
    make_plot(
        'coproc_geomean.pdf',
        'Geomean Slowdown',
        ["SATBlast", "Coproc"],
        [geomean_slowdowns[c] for c in luby_only],
        [COLORS[c] for c in luby_only],
    )


if __name__ == "__main__":
    main()
