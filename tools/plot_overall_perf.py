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
import math
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

# Cycle-domain reference frequency: the uniform timeout budget is anchored here.
# timeout_seconds * CYCLE_REF_HZ cycles (default 36 s * 1 GHz = 36 Gcycles).
CYCLE_REF_HZ = 1e9


def get_folder_frequency(name, default_hz=250e6):
    """Infer a solver's clock frequency (Hz) from its display name.

    Used by the cycle-domain plots to convert wall-clock runtimes into clock cycles.
    Assumed clocks: Kissat/MiniSAT = 5 GHz, SATBlast = 1 GHz, Baseline = 1 GHz,
    everything else (SAT-Accel / our accelerator) = 250 MHz. At 250 MHz the cycle-domain
    factor (1 GHz ref / 250 MHz = 4) matches the wall-clock --normalize-sataccel /4.
    """
    n = name.lower()
    if 'kissat' in n or 'minisat' in n:
        return 5e9
    if 'satblast' in n or 'baseline' in n:
        return 1e9
    return default_hz


def compute_par2_cycles_on_shared_set(folder_metrics, freq_map, shared_tests,
                                      timeout_seconds, ref_freq_hz=CYCLE_REF_HZ,
                                      errors_as_timeout=False):
    """Compute PAR-2 in clock cycles (returned in Gcycles) on the shared test set.

    Each instance's cycle count = runtime_s * solver_frequency. The timeout budget is a
    single uniform value anchored at ref_freq_hz: timeout_seconds * ref_freq_hz
    (default 36 s * 1 GHz = 36 Gcycles). Any instance whose cycle count exceeds this
    budget, or that timed out in wall-clock, is treated as a timeout and charged 2x the
    budget. 'solved' counts instances finishing within the cycle budget.

    Returns: dict folder_name -> (par2_gcycles, solved_count, total_count).
    """
    wall_timeout_ms = timeout_seconds * 1000.0
    timeout_cycles = timeout_seconds * ref_freq_hz
    penalty_cycles = 2 * timeout_cycles

    scores = {}
    for folder_name, metrics in folder_metrics.items():
        freq = freq_map.get(folder_name, 250e6)
        if errors_as_timeout:
            shared_results = [r for r in metrics['results']
                              if r.get('test_case') in shared_tests]
        else:
            shared_results = [r for r in metrics['results']
                              if r.get('test_case') in shared_tests
                              and r.get('result') not in ('ERROR', 'UNKNOWN')]
        if not shared_results:
            scores[folder_name] = (None, 0, 0)
            continue

        total_cycles = 0.0
        solved = 0
        for r in shared_results:
            result = r.get('result', 'UNKNOWN')
            primary = result.split()[0] if result else 'UNKNOWN'
            try:
                # Cycle domain uses the TRUE runtime; the /4 SatAccel wall-clock
                # normalization must not be applied again here.
                sim_ms = float(r.get('sim_time_ms_true', r.get('sim_time_ms', 0.0)) or 0.0)
            except (TypeError, ValueError):
                sim_ms = 0.0
            if errors_as_timeout and primary in ('ERROR', 'UNKNOWN'):
                total_cycles += timeout_cycles  # 1x penalty for errors
            elif primary in ('SAT', 'UNSAT') and sim_ms <= wall_timeout_ms:
                cyc = (sim_ms / 1000.0) * freq
                if cyc <= timeout_cycles:
                    total_cycles += cyc
                    solved += 1
                else:
                    total_cycles += penalty_cycles
            else:  # TIMEOUT or over wall-clock limit
                total_cycles += penalty_cycles

        par2_gcycles = (total_cycles / len(shared_results)) / 1e9
        scores[folder_name] = (par2_gcycles, solved, len(shared_results))

    return scores


def compute_geomean_speedups_cycles(folder_metrics, freq_map, shared_tests,
                                    timeout_seconds, baseline_name, ref_freq_hz=CYCLE_REF_HZ,
                                    errors_as_timeout=False):
    """Geometric mean speedup in the cycle domain vs baseline on the shared test set.

    Speedup per test = base_cycles / config_cycles, using the same uniform cycle budget
    and penalty as compute_par2_cycles_on_shared_set. Baseline has speedup 1.0.
    Returns: dict folder_name -> geomean_speedup (float or None).
    """
    wall_timeout_ms = timeout_seconds * 1000.0
    timeout_cycles = timeout_seconds * ref_freq_hz
    penalty_cycles = 2 * timeout_cycles
    error_penalty = timeout_cycles if errors_as_timeout else penalty_cycles

    def eff_cycles(r, freq):
        result = r.get('result', 'UNKNOWN')
        primary = result.split()[0] if result else 'UNKNOWN'
        try:
            # Cycle domain uses the TRUE runtime; the /4 SatAccel wall-clock
            # normalization must not be applied again here.
            sim_ms = float(r.get('sim_time_ms_true', r.get('sim_time_ms', 0.0)) or 0.0)
        except (TypeError, ValueError):
            sim_ms = 0.0
        if primary in ('SAT', 'UNSAT') and sim_ms <= wall_timeout_ms:
            cyc = (sim_ms / 1000.0) * freq
            return cyc if cyc <= timeout_cycles else penalty_cycles
        if errors_as_timeout and primary in ('ERROR', 'UNKNOWN'):
            return error_penalty
        return penalty_cycles

    folder_case_map = {}
    for folder_name, metrics in folder_metrics.items():
        freq = freq_map.get(folder_name, 250e6)
        case_map = {}
        for r in metrics['results']:
            tc = r.get('test_case')
            if tc not in shared_tests:
                continue
            case_map[tc] = eff_cycles(r, freq)
        folder_case_map[folder_name] = case_map

    geomeans = {}
    baseline_map = folder_case_map.get(baseline_name, {})
    for folder_name in folder_metrics.keys():
        if folder_name == baseline_name:
            geomeans[folder_name] = 1.0
            continue
        case_map = folder_case_map.get(folder_name, {})
        ln_sum = 0.0
        n = 0
        for tc in shared_tests:
            tb = baseline_map.get(tc)
            tc_eff = case_map.get(tc)
            if not tb or not tc_eff or tb <= 0 or tc_eff <= 0:
                continue
            ln_sum += math.log(tb / tc_eff)
            n += 1
        geomeans[folder_name] = math.exp(ln_sum / n) if n > 0 else None

    return geomeans


def generate_figure(folder_metrics, folder_names, baseline_name, shared_tests,
                    timeout_seconds, errors_as_timeout, exclude_timeouts, pdf_path,
                    freq_map=None):
    """Generate a single (a)+(b) PDF for the given shared test set.

    When freq_map is provided, the (a) panel shows PAR-2 in Gcycles and (b) shows the
    frequency-normalized (cycle-domain) geomean speedup instead of wall-clock values.
    """
    cycles = freq_map is not None
    unit = 'Gcyc' if cycles else 's'
    if cycles:
        shared_par2 = compute_par2_cycles_on_shared_set(
            folder_metrics, freq_map, shared_tests, timeout_seconds,
            errors_as_timeout=errors_as_timeout)
        geomeans = compute_geomean_speedups_cycles(
            folder_metrics, freq_map, shared_tests, timeout_seconds, baseline_name,
            errors_as_timeout=errors_as_timeout)
    else:
        shared_par2 = compute_par2_on_shared_set(folder_metrics, shared_tests,
                                                  timeout_seconds, errors_as_timeout)
        geomeans = compute_geomean_speedups(folder_metrics, shared_tests,
                                             timeout_seconds, baseline_name, errors_as_timeout)

    tag = "cycles" if cycles else ("no-timeouts" if exclude_timeouts else "with-timeouts")
    par2_col = 'PAR-2 (Gcyc)' if cycles else 'PAR-2 (s)'
    print(f"\n[{tag}] {len(shared_tests)} shared tests")
    print(f"{'Folder':<30} {par2_col:<14} {'Solved/Total':<16} {'Geomean×':<12}")
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
        if cycles:
            y_label = 'PAR-2 (Gcycles)'
        else:
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
            # Cycle plot drops the unit suffix in bar labels (unit is on the y-axis);
            # wall-clock keeps "s".
            val_label = f'{par2:.2f}' if cycles else f'{par2:.2f} {unit}'
            if exclude_timeouts:
                bar_label = val_label
            else:
                bar_label = f'{val_label}\nTO: {timeout_count}'
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

    # --- PDF 3: frequency-normalized (cycle domain), uses the with-timeouts set ---
    freq_map = {name: get_folder_frequency(name) for name in folder_metrics}
    print("\nCycle view assumed clocks (from name match):")
    for name, hz in freq_map.items():
        print(f"  {name:<30} {hz/1e9:g} GHz")
    print(f"Cycle timeout budget: {args.timeout * CYCLE_REF_HZ / 1e9:g} Gcycles "
          f"({args.timeout:g} s x {CYCLE_REF_HZ/1e9:g} GHz)")
    if shared_tests:
        generate_figure(folder_metrics, folder_names, baseline_name, shared_tests,
                        args.timeout, args.errors_as_timeout, False,
                        output_dir / 'overall_perf_cycle.pdf', freq_map=freq_map)
    else:
        print("Error: No shared tests for cycle set")


if __name__ == '__main__':
    main()
