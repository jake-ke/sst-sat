#!/usr/bin/env python3
"""
Speedup Scatter Plot: compare two folders with scatter plots of speedup
over #variables, #clauses, and baseline runtime.

Modes:
  Default:    Y-axis = speedup (baseline_time / compare_time)
  --runtime:  Y-axis = runtime (ms), both folders overlaid with different colors

Usage:
    python3 tools/plot_speedup_scatter.py <baseline_folder> <compare_folder> \
        [--names BASE CMP] [--timeout 36] [--output-dir results/] \
        [--errors-as-timeout] [--large-fonts] [--log-y] [--runtime]
"""

import sys
import math
import argparse
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

from unified_parser import parse_log_directory
from compute_coprocessor import compute_coprocessor


# ── Manual exclusions (same as plot_comparison.py) ───────────────────────────
MANUAL_EXCLUSIONS = {
    "080896c437245ac25eb6d3ad6df12c4f-bv-term-small-rw_1492.smt2.cnf",
    "e17d3f94f2c0e11ce6143bc4bf298bd7-mp1-qpr-bmp280-driver-5.cnf",
    "e185ebbd92c23ab460a3d29046eccf1d-group_mulr.cnf",
}


def normalize_test_case(name):
    if not name:
        return name
    known_exts = {'.cnf', '.dimacs', '.smt2', '.xml', '.c', '.txt', '.log'}
    lower = name.lower()
    for ext in known_exts:
        if lower.endswith(ext):
            return name[:-(len(ext))]
    return name


def _apply_coproc_scaling(raw_result, roundtrip, sf_scale):
    """Scale sim_time_ms by coproc/hw cycle ratio. Returns scaled ms or None if no coproc data."""
    has_coproc = any(k.startswith('coproc_') for k in raw_result)
    if not has_coproc:
        return None
    coproc, hw = compute_coprocessor(raw_result, roundtrip, sf_scale)
    hw_total = sum(hw.values())
    coproc_total = sum(coproc.values())
    if hw_total <= 0:
        return None
    ratio = coproc_total / hw_total
    sim_ms = float(raw_result.get('sim_time_ms', 0.0) or 0.0)
    return sim_ms * ratio


def load_folder(folder_path, timeout_seconds, errors_as_timeout=False,
                coproc=False, coproc_roundtrip=10, coproc_sf_scale=1.0):
    """Load results from a folder (single or multi-seed), apply timeout classification.
    If coproc=True, scale sim_time_ms by coprocessor/hw cycle ratio.
    Returns dict mapping normalized test_case -> {sim_time_ms, variables, clauses, result}."""
    folder_path = Path(folder_path)
    timeout_ms = timeout_seconds * 1000.0

    # Check for multi-seed layout
    seed_dirs = [p for p in sorted(folder_path.glob('seed*')) if p.is_dir()]

    if seed_dirs:
        seed_results = []
        for sd in seed_dirs:
            res = parse_log_directory(sd, exclude_summary=True)
            if res:
                seed_results.append(res)
        if not seed_results:
            return {}

        # Build per-seed maps
        seed_maps = []
        for res in seed_results:
            m = {r.get('test_case', ''): r for r in res}
            seed_maps.append(m)

        all_cases = set()
        for m in seed_maps:
            all_cases.update(m.keys())

        results = {}
        for case in sorted(all_cases):
            entries = [m[case] for m in seed_maps if case in m]
            if not entries:
                continue

            # Aggregate result label
            labels = [e.get('result', 'UNKNOWN') for e in entries]
            unique = set(labels)
            if len(unique) == 1:
                result = labels[0]
            elif 'TIMEOUT' in unique:
                result = 'TIMEOUT'
            elif 'ERROR' in unique or 'UNKNOWN' in unique:
                result = 'ERROR'
            else:
                result = labels[0]  # mixed SAT/UNSAT — use first

            # Average sim_time_ms across seeds (use timeout for TIMEOUT seeds)
            time_vals = []
            for e in entries:
                r = e.get('result', 'UNKNOWN')
                if r in ('ERROR', 'UNKNOWN'):
                    continue
                if r == 'TIMEOUT':
                    time_vals.append(timeout_ms)
                else:
                    sim_ms = float(e.get('sim_time_ms', 0.0) or 0.0)
                    if coproc:
                        scaled = _apply_coproc_scaling(e, coproc_roundtrip, coproc_sf_scale)
                        if scaled is not None:
                            sim_ms = scaled
                    if sim_ms > timeout_ms:
                        time_vals.append(timeout_ms)
                    else:
                        time_vals.append(sim_ms)
            avg_time = sum(time_vals) / len(time_vals) if time_vals else timeout_ms

            # Variables/clauses (take from first entry)
            variables = int(entries[0].get('variables', 0) or 0)
            clauses = int(entries[0].get('clauses', 0) or 0)

            total_memory = int(entries[0].get('total_memory_bytes', 0) or 0)

            norm = normalize_test_case(case)
            results[norm] = {
                'sim_time_ms': avg_time,
                'variables': variables,
                'clauses': clauses,
                'total_memory_bytes': total_memory,
                'result': result,
            }
        return results
    else:
        # Single-run folder
        raw = parse_log_directory(folder_path, exclude_summary=True)
        if not raw:
            return {}

        results = {}
        for r in raw:
            res = r.get('result', 'UNKNOWN')
            try:
                sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
            except (TypeError, ValueError):
                sim_ms = 0.0

            if coproc:
                scaled = _apply_coproc_scaling(r, coproc_roundtrip, coproc_sf_scale)
                if scaled is not None:
                    sim_ms = scaled

            # Reclassify if over timeout
            if res in ('SAT', 'UNSAT') and sim_ms > timeout_ms:
                res = 'TIMEOUT'
            if res == 'TIMEOUT':
                sim_ms = timeout_ms

            norm = normalize_test_case(r.get('test_case', ''))
            results[norm] = {
                'sim_time_ms': sim_ms,
                'variables': int(r.get('variables', 0) or 0),
                'clauses': int(r.get('clauses', 0) or 0),
                'total_memory_bytes': int(r.get('total_memory_bytes', 0) or 0),
                'result': res,
            }
        return results


def main():
    parser = argparse.ArgumentParser(description='Scatter plot of speedup vs problem size')
    parser.add_argument('baseline', help='Baseline folder')
    parser.add_argument('compare', help='Comparison folder')
    parser.add_argument('--names', nargs=2, default=None,
                        help='Display names for baseline and compare folders')
    parser.add_argument('--timeout', type=float, default=36,
                        help='Timeout in seconds (default: 36)')
    parser.add_argument('--output-dir', default='results',
                        help='Directory to save output plots')
    parser.add_argument('--exclude-timeouts', action='store_true',
                        help='Exclude tests where either folder timed out')
    parser.add_argument('--errors-as-timeout', action='store_true',
                        help='Treat ERROR/UNKNOWN as timeout instead of excluding')
    parser.add_argument('--large-fonts', action='store_true',
                        help='Use larger font sizes (3x scale)')
    parser.add_argument('--log-y', action='store_true',
                        help='Use log scale on Y-axis (speedup)')
    parser.add_argument('--runtime', action='store_true',
                        help='Plot runtime of both folders instead of speedup')
    parser.add_argument('--coproc', action='store_true',
                        help='Scale compare folder runtime by coprocessor/HW cycle ratio')
    parser.add_argument('--coproc-roundtrip', type=int, default=10,
                        help='Coprocessor round-trip latency in accel cycles (default: 10)')
    parser.add_argument('--coproc-sf-scale', type=float, default=1.0,
                        help='Coprocessor serialization factor scale (default: 1.0)')
    args = parser.parse_args()

    base_name = args.names[0] if args.names else Path(args.baseline).name
    cmp_name = args.names[1] if args.names else Path(args.compare).name

    print(f"Loading baseline: {args.baseline}")
    base_data = load_folder(args.baseline, args.timeout, args.errors_as_timeout)
    print(f"  Loaded {len(base_data)} tests")

    print(f"Loading compare: {args.compare}" + (" (coproc scaling)" if args.coproc else ""))
    cmp_data = load_folder(args.compare, args.timeout, args.errors_as_timeout,
                           coproc=args.coproc, coproc_roundtrip=args.coproc_roundtrip,
                           coproc_sf_scale=args.coproc_sf_scale)
    print(f"  Loaded {len(cmp_data)} tests")

    # Normalized exclusions
    norm_exclusions = {normalize_test_case(n) for n in MANUAL_EXCLUSIONS}

    # Find shared tests
    shared = set(base_data.keys()) & set(cmp_data.keys())
    shared -= norm_exclusions

    # Build scatter data
    variables = []
    clauses = []
    memory_bytes = []
    base_times = []
    cmp_times = []
    speedups = []
    colors = []
    timeout_ms = args.timeout * 1000.0
    penalty_ms = 2 * timeout_ms  # PAR-2: 2× timeout for TIMEOUT
    error_penalty_ms = timeout_ms if args.errors_as_timeout else penalty_ms

    for tc in sorted(shared):
        b = base_data[tc]
        c = cmp_data[tc]

        # Skip ERROR/UNKNOWN unless errors_as_timeout
        if not args.errors_as_timeout:
            if b['result'] in ('ERROR', 'UNKNOWN') or c['result'] in ('ERROR', 'UNKNOWN'):
                continue

        # Compute PAR-2 effective times (matches plot_comparison.py)
        if b['result'] in ('SAT', 'UNSAT') and b['sim_time_ms'] <= timeout_ms:
            b_time = b['sim_time_ms']
        elif args.errors_as_timeout and b['result'] in ('ERROR', 'UNKNOWN'):
            b_time = error_penalty_ms
        else:
            b_time = penalty_ms

        if c['result'] in ('SAT', 'UNSAT') and c['sim_time_ms'] <= timeout_ms:
            c_time = c['sim_time_ms']
        elif args.errors_as_timeout and c['result'] in ('ERROR', 'UNKNOWN'):
            c_time = error_penalty_ms
        else:
            c_time = penalty_ms

        # Skip timeouts
        b_is_timeout = b['result'] not in ('SAT', 'UNSAT') or b['sim_time_ms'] > timeout_ms
        c_is_timeout = c['result'] not in ('SAT', 'UNSAT') or c['sim_time_ms'] > timeout_ms
        if args.exclude_timeouts and (b_is_timeout or c_is_timeout):
            continue
        # Skip if both timed out (speedup undefined)
        if not args.runtime and b_is_timeout and c_is_timeout:
            continue

        # Avoid division by zero
        if c_time <= 0:
            c_time = 0.1
        if b_time <= 0:
            b_time = 0.1

        speedup = b_time / c_time

        variables.append(b['variables'])
        clauses.append(b['clauses'])
        memory_bytes.append(b['total_memory_bytes'])
        base_times.append(b_time)
        cmp_times.append(c_time)
        speedups.append(speedup)

        colors.append('#1f77b4')

    if not speedups:
        print("No shared tests with valid data. Nothing to plot.")
        sys.exit(1)

    # Geomean speedup
    log_speedups = [math.log(s) for s in speedups]
    geomean = math.exp(sum(log_speedups) / len(log_speedups))

    print(f"\nShared tests plotted: {len(speedups)}")
    print(f"Geomean speedup: {geomean:.3f}x")

    # ── Plotting ─────────────────────────────────────────────────────────────
    from matplotlib.lines import Line2D
    font_scale = 2.0 if args.large_fonts else 0.6
    fig_w = 12 * (font_scale * 0.4 + 0.6)
    fig_h = 8 * (font_scale * 0.4 + 0.6)
    fig, axes = plt.subplots(2, 2, figsize=(fig_w, fig_h))
    axes = axes.flatten()

    marker_size = 20 * font_scale
    base_color = '#d62728'   # red for baseline
    cmp_color = '#1f77b4'    # blue for compare

    # Convert memory to KiB for readability
    memory_kib = [m / 1024.0 if m > 0 else 0 for m in memory_bytes]

    if args.runtime:
        # ── Runtime mode: overlay both folders' runtimes ─────────────────
        x_data = [
            (np.array(variables), 'Variables'),
            (np.array(clauses), 'Clauses'),
            (np.array(memory_kib), 'Memory Footprint (KiB)'),
        ]
        # Fourth subplot: sorted test index
        x_indices = np.arange(len(base_times))
        sort_order = np.argsort(base_times)

        for ax, (xvals, xlabel) in zip(axes[:3], x_data):
            ax.scatter(xvals, base_times, c=base_color, s=marker_size, alpha=0.6,
                       edgecolors='none', label=base_name)
            ax.scatter(xvals, cmp_times, c=cmp_color, s=marker_size, alpha=0.6,
                       edgecolors='none', label=cmp_name)
            ax.set_xscale('log')
            ax.xaxis.set_major_locator(ticker.LogLocator(base=10, numticks=10))
            ax.xaxis.set_major_formatter(ticker.LogFormatterMathtext())
            ax.xaxis.set_minor_formatter(ticker.NullFormatter())
            ax.set_yscale('log')
            ax.set_xlabel(xlabel, fontsize=int(18 * font_scale))
            ax.set_ylabel('Runtime (ms)', fontsize=int(18 * font_scale))
            ax.tick_params(labelsize=int(14 * font_scale))
            ax.grid(True, alpha=0.3)

        # Fourth panel: paired runtime sorted by baseline
        sorted_base = np.array(base_times)[sort_order]
        sorted_cmp = np.array(cmp_times)[sort_order]
        axes[3].scatter(x_indices, sorted_base, c=base_color, s=marker_size, alpha=0.6,
                        edgecolors='none', label=base_name)
        axes[3].scatter(x_indices, sorted_cmp, c=cmp_color, s=marker_size, alpha=0.6,
                        edgecolors='none', label=cmp_name)
        axes[3].set_yscale('log')
        axes[3].set_xlabel('Test (sorted by baseline runtime)', fontsize=int(18 * font_scale))
        axes[3].set_ylabel('Runtime (ms)', fontsize=int(18 * font_scale))
        axes[3].tick_params(labelsize=int(14 * font_scale))
        axes[3].grid(True, alpha=0.3)

        # Legend on last subplot
        legend_elements = [
            Line2D([0], [0], marker='o', color='w', markerfacecolor=base_color,
                   markersize=8 * font_scale**0.5, label=base_name),
            Line2D([0], [0], marker='o', color='w', markerfacecolor=cmp_color,
                   markersize=8 * font_scale**0.5, label=cmp_name),
        ]
        axes[3].legend(handles=legend_elements, loc='upper left',
                       fontsize=int(9 * font_scale), frameon=True)

        fig.suptitle(f'Runtime: {base_name} vs {cmp_name}  (n={len(base_times)}, geomean speedup={geomean:.2f}x)',
                     fontsize=int(14 * font_scale))
        out_name = 'runtime_scatter'
    else:
        # ── Speedup mode ────────────────────────────────────────────────
        x_data = [
            (np.array(variables), 'Variables'),
            (np.array(clauses), 'Clauses'),
            (np.array(memory_kib), 'Memory Footprint (KiB)'),
            (np.array(base_times), f'{base_name} Runtime (ms)'),
        ]

        # Compute global x-range across all datasets
        all_x = np.concatenate([xv[xv > 0] for xv, _ in x_data])
        global_min = 10 ** math.floor(math.log10(all_x.min()))
        global_max = 10 ** math.ceil(math.log10(all_x.max()))

        for ax, (xvals, xlabel) in zip(axes, x_data):
            ax.scatter(xvals, speedups, c=colors, s=marker_size, alpha=0.7, edgecolors='none')
            ax.set_xscale('log')
            ax.set_xlim(global_min, global_max)
            ax.xaxis.set_major_locator(ticker.LogLocator(base=10, numticks=10))
            ax.xaxis.set_major_formatter(ticker.LogFormatterMathtext())
            ax.xaxis.set_minor_formatter(ticker.NullFormatter())
            if args.log_y:
                ax.set_yscale('log')
            ax.set_xlabel(xlabel, fontsize=int(18 * font_scale))
            ax.set_ylabel('Speedup', fontsize=int(18 * font_scale))
            ax.tick_params(labelsize=int(14 * font_scale))
            ax.grid(True, alpha=0.3)

        out_name = 'speedup_scatter'

    fig.tight_layout()

    # Save
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f'{out_name}.pdf'
    fig.savefig(out_path, dpi=200, bbox_inches='tight')
    print(f"Saved: {out_path}")

    # ── Single-panel: speedup over runtime with trend line ───────────
    if not args.runtime:
        fs = int(20 * font_scale)
        fs_tick = int(16 * font_scale)
        fig2, ax2 = plt.subplots(figsize=(7 * (font_scale * 0.4 + 0.6),
                                          2.5 * (font_scale * 0.4 + 0.6)))
        x_rt = np.array(base_times)
        y_sp = np.array(speedups)
        ax2.scatter(x_rt, y_sp, c='#1f77b4', s=marker_size * 1.5, alpha=0.7, edgecolors='none')
        ax2.set_xscale('log')
        if args.log_y:
            ax2.set_yscale('log')

        # Trend line (linear fit in log-x space)
        log_x = np.log10(x_rt)
        coeffs = np.polyfit(log_x, y_sp, 1)
        x_sorted = np.sort(log_x)
        ax2.plot(10**x_sorted, np.polyval(coeffs, x_sorted),
                 color='#66bb6a', linewidth=2, linestyle='--', alpha=0.5, zorder=5)

        ax2.set_xlabel(f'{base_name} Runtime (ms)', fontsize=fs)
        ax2.set_ylabel('Speedup', fontsize=fs)
        ax2.tick_params(labelsize=fs_tick)
        ax2.grid(True, alpha=0.3)
        fig2.tight_layout()

        out_path2 = out_dir / 'speedup_vs_runtime.pdf'
        fig2.savefig(out_path2, dpi=200, bbox_inches='tight')
        print(f"Saved: {out_path2}")
        plt.close(fig2)

    plt.close(fig)


if __name__ == '__main__':
    main()
