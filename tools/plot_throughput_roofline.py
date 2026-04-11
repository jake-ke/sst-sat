#!/usr/bin/env python3
"""
SAT Solver Throughput and Roofline Comparison Plotter

Compares two configurations (baseline vs accelerator) using:
1. Propagations/s comparison bar chart
2. Roofline plot (operational intensity vs throughput)
3. Bandwidth utilization comparison bar chart

Uses L1 cache miss data for bandwidth estimation (cache-aware roofline model).
L1 misses represent the data movement demand from the accelerator to the memory hierarchy.

Usage: python plot_throughput_roofline.py <folder1> <folder2> --peak-bandwidth <GB/s> --peak-compute <props/s>
       [--names "Baseline" "SATBlast"] [--timeout 36] [--output-dir results/]

Examples:
    python plot_throughput_roofline.py runs/baseline runs/optimized --peak-bandwidth 16 --peak-compute 1e9
    python plot_throughput_roofline.py logs_base logs_opt --names "Baseline" "SATBlast" --peak-bandwidth 16 --peak-compute 1e9
"""

import sys
import argparse
import math
import numpy as np
from pathlib import Path
import matplotlib.pyplot as plt
from unified_parser import parse_log_directory

CACHE_LINE_SIZE = 64  # bytes

# Manual exclusion list (same as plot_comparison.py)
MANUAL_EXCLUSIONS = set([
    "080896c437245ac25eb6d3ad6df12c4f-bv-term-small-rw_1492.smt2.cnf",
    "e17d3f94f2c0e11ce6143bc4bf298bd7-mp1-qpr-bmp280-driver-5.cnf",
    "e185ebbd92c23ab460a3d29046eccf1d-group_mulr.cnf",
])


def get_folder_colors(folder_names):
    """Assign colors to folders with custom logic:
    - First folder (baseline): Red
    - "SATBlast" folder: Blue
    - Other folders: Colors in spectrum order
    """
    spectrum_colors = [
        '#ff7f0e',  # Orange
        '#bcbd22',  # Yellow-green
        '#2ca02c',  # Green
        '#17becf',  # Cyan
        '#9467bd',  # Purple
        '#e377c2',  # Pink
        '#8c564b',  # Brown
        '#7f7f7f',  # Gray
    ]

    colors = []
    spectrum_idx = 0

    for idx, name in enumerate(folder_names):
        if idx == 0:
            colors.append('#d62728')
        elif 'SATBlast' in name or 'satblast' in name.lower():
            colors.append('#1f77b4')
        else:
            colors.append(spectrum_colors[spectrum_idx % len(spectrum_colors)])
            spectrum_idx += 1

    return colors


def compute_metrics(results, timeout_ms):
    """Compute per-benchmark throughput and bandwidth metrics.

    Uses L1 cache misses for bandwidth estimation (cache-aware roofline).
    L1 misses represent data moved from L1 to the rest of the memory hierarchy,
    which is the relevant bandwidth constraint for the accelerator.

    Args:
        results: list of parsed result dicts from unified_parser
        timeout_ms: timeout in milliseconds for PAR-2 filtering

    Returns:
        dict mapping test_case -> {propagations_per_sec, actual_bandwidth_GBs,
                                    operational_intensity, propagations, sim_time_ms,
                                    l1_total_misses, mem_bytes}
    """
    metrics = {}

    for r in results:
        test_case = r.get('test_case', '')
        result_status = r.get('result')

        # Skip ERROR/UNKNOWN
        if result_status in ('ERROR', 'UNKNOWN'):
            continue

        propagations = r.get('propagations', 0) or 0
        sim_time_ms = r.get('sim_time_ms', 0) or 0

        # Compute L1 total misses and requests from per-component data
        l1_components = ['heap', 'variables', 'watches', 'clauses', 'varactivity']
        l1_total_misses = sum(r.get(f'l1_{comp}_misses', 0) or 0 for comp in l1_components)
        l1_total_hits = sum(r.get(f'l1_{comp}_hits', 0) or 0 for comp in l1_components)
        l1_total_requests = l1_total_hits + l1_total_misses

        # Skip if missing essential data
        if propagations <= 0 or sim_time_ms <= 0:
            continue

        if l1_total_misses <= 0:
            print(f"  Warning: Skipping {test_case} — no L1 miss data")
            continue

        # Skip timeouts
        if sim_time_ms > timeout_ms:
            continue

        sim_time_s = sim_time_ms / 1000.0
        mem_bytes = l1_total_misses * CACHE_LINE_SIZE
        propagations_per_sec = propagations / sim_time_s
        actual_bandwidth_GBs = mem_bytes / sim_time_s / 1e9
        operational_intensity = propagations / mem_bytes
        l1_miss_rate = (l1_total_misses / l1_total_requests * 100.0) if l1_total_requests > 0 else 0.0
        req_per_prop = l1_total_requests / propagations

        metrics[test_case] = {
            'propagations_per_sec': propagations_per_sec,
            'actual_bandwidth_GBs': actual_bandwidth_GBs,
            'operational_intensity': operational_intensity,
            'propagations': propagations,
            'sim_time_ms': sim_time_ms,
            'l1_total_misses': l1_total_misses,
            'l1_total_requests': l1_total_requests,
            'l1_miss_rate': l1_miss_rate,
            'req_per_prop': req_per_prop,
            'mem_bytes': mem_bytes,
        }

    return metrics


def get_font_sizes():
    """Return font size dict for publication-quality figures."""
    return {
        'axis_label': 40,
        'axis_label_weight': 'bold',
        'tick': 36,
        'legend': 46,
        'annotation': 38,
    }


def plot_propagations_per_sec(common_tests, all_metrics, folder_names, colors, fonts, output_path):
    """Plot propagations/s comparison bar chart, sorted by speedup ratio."""
    # Compute speedup for sorting
    baseline_name = folder_names[0]
    accel_name = folder_names[1]

    test_speedups = []
    for tc in common_tests:
        base_ps = all_metrics[baseline_name][tc]['propagations_per_sec']
        accel_ps = all_metrics[accel_name][tc]['propagations_per_sec']
        speedup = accel_ps / base_ps if base_ps > 0 else 1.0
        test_speedups.append((tc, speedup))

    # Sort by speedup (ascending)
    test_speedups.sort(key=lambda x: x[1])
    sorted_tests = [tc for tc, _ in test_speedups]

    # Compute geometric mean speedup
    log_speedups = [math.log(s) for _, s in test_speedups if s > 0]
    geomean_speedup = math.exp(sum(log_speedups) / len(log_speedups)) if log_speedups else 1.0

    labels = [tc[:6] for tc in sorted_tests]

    fig_width = max(12, len(sorted_tests) * 0.5)
    fig, ax = plt.subplots(figsize=(fig_width, 6.5))

    num_folders = len(folder_names)
    bar_width = 0.8 / num_folders
    x_base = range(len(sorted_tests))

    for folder_idx, folder_name in enumerate(folder_names):
        x_positions = [x + folder_idx * bar_width for x in x_base]
        values = [all_metrics[folder_name][tc]['propagations_per_sec'] for tc in sorted_tests]

        ax.bar(x_positions, values, bar_width,
               label=folder_name,
               color=colors[folder_idx],
               alpha=0.85, edgecolor='black', linewidth=0.8)

    ax.set_ylabel('Propagations\nper Second', fontsize=fonts['axis_label'], fontweight=fonts['axis_label_weight'])
    ax.set_xticks([x + bar_width * (num_folders - 1) / 2 for x in x_base])
    ax.set_xticklabels(labels, fontsize=20, rotation=45, ha='right')
    ax.tick_params(axis='y', labelsize=fonts['tick'])
    ax.set_xlim(-0.5, len(sorted_tests) - 0.2)

    ax.grid(axis='y', alpha=0.6, linestyle='-', linewidth=1.2)
    ax.set_axisbelow(True)

    # Annotation for geometric mean
    ax.text(0.02, 0.95, f'Geomean speedup: {geomean_speedup:.2f}x',
            transform=ax.transAxes, fontsize=fonts['annotation'],
            verticalalignment='top', bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.8))

    # Legend
    max_val = max(all_metrics[fn][tc]['propagations_per_sec'] for fn in folder_names for tc in sorted_tests)
    ax.set_ylim(0, max_val * 1.35)
    ax.legend(loc='upper center', fontsize=fonts['legend'], frameon=True,
              ncol=num_folders, bbox_to_anchor=(0.5, 1.02),
              handlelength=1.0, handletextpad=0.5, columnspacing=1.0)

    plt.tight_layout()
    plt.savefig(output_path, format='pdf', bbox_inches='tight')
    print(f"Propagations/s chart saved to: {output_path}")
    plt.close(fig)


def plot_roofline(common_tests, all_metrics, folder_names, colors, fonts,
                  peak_bandwidth_GBs, peak_compute_props, output_path):
    """Plot roofline chart with both configs."""
    fig, ax = plt.subplots(figsize=(12, 9))

    # Collect all OI values to set axis range
    all_oi = []
    all_perf = []

    for folder_idx, folder_name in enumerate(folder_names):
        oi_vals = []
        perf_vals = []
        for tc in common_tests:
            m = all_metrics[folder_name][tc]
            oi_vals.append(m['operational_intensity'])
            perf_vals.append(m['propagations_per_sec'])

        ax.scatter(oi_vals, perf_vals, color=colors[folder_idx], label=folder_name,
                   s=120, alpha=0.8, edgecolors='black', linewidth=0.5, zorder=3)
        all_oi.extend(oi_vals)
        all_perf.extend(perf_vals)

    # Draw roofline ceilings
    if all_oi:
        oi_min = min(all_oi) * 0.3
        oi_max = max(all_oi) * 3.0

        # Ridge point: where bandwidth ceiling meets compute ceiling
        peak_bw_bytes = peak_bandwidth_GBs * 1e9  # bytes/s
        ridge_oi = peak_compute_props / peak_bw_bytes

        # Bandwidth ceiling: performance = peak_bw_bytes * OI
        bw_oi_range = [oi_min, min(ridge_oi, oi_max)]
        bw_perf = [peak_bw_bytes * oi for oi in bw_oi_range]
        ax.plot(bw_oi_range, bw_perf, 'k--', linewidth=2.5, zorder=2)

        # Compute ceiling: horizontal line at peak compute
        compute_oi_range = [max(ridge_oi, oi_min), oi_max]
        ax.plot(compute_oi_range, [peak_compute_props, peak_compute_props],
                'k--', linewidth=2.5, zorder=2)

        # Labels for ceilings (placed after axis setup below)
        ax.text(compute_oi_range[-1] * 0.7, peak_compute_props * 1.15,
                f'Peak Compute: {peak_compute_props:.2e} prop/s',
                fontsize=fonts['annotation'], ha='right', va='bottom')

    ax.set_xscale('log')
    ax.set_yscale('log')
    ax.set_xlabel('Operational Intensity (propagations/byte)',
                  fontsize=fonts['axis_label'], fontweight=fonts['axis_label_weight'])
    ax.set_ylabel('Propagations/s',
                  fontsize=fonts['axis_label'], fontweight=fonts['axis_label_weight'])
    ax.tick_params(axis='both', labelsize=fonts['tick'])
    ax.grid(True, alpha=0.3, linestyle='--', which='both')
    ax.set_axisbelow(True)

    # Extend y-axis upper limit to fit peak compute label
    y_lo, y_hi = ax.get_ylim()
    ax.set_ylim(y_lo, y_hi * 3)

    # Compute BW label rotation to match the line's visual angle on log-log axes
    if all_oi:
        fig.canvas.draw()
        # Transform two points on the BW line to display coordinates
        p1 = ax.transData.transform((bw_oi_range[0], bw_perf[0]))
        p2 = ax.transData.transform((bw_oi_range[-1], bw_perf[-1]))
        angle_deg = np.degrees(np.arctan2(p2[1] - p1[1], p2[0] - p1[0]))
        ax.text(bw_oi_range[0] * 1.2, bw_perf[0] * 1.3,
                f'Peak BW: {peak_bandwidth_GBs:.1f} GB/s',
                fontsize=fonts['annotation'], rotation=angle_deg,
                rotation_mode='anchor', va='bottom')

    ax.legend(fontsize=fonts['legend'], frameon=True, loc='lower right')

    plt.tight_layout()
    plt.savefig(output_path, format='pdf', bbox_inches='tight')
    print(f"Roofline plot saved to: {output_path}")
    plt.close(fig)


def plot_bandwidth_comparison(common_tests, all_metrics, folder_names, colors, fonts, output_path):
    """Plot bandwidth utilization comparison bar chart."""
    # Sort by baseline bandwidth (ascending)
    baseline_name = folder_names[0]
    accel_name = folder_names[1]
    sorted_tests = sorted(common_tests,
                          key=lambda tc: all_metrics[baseline_name][tc]['actual_bandwidth_GBs'])

    # Compute geomean bandwidth ratio
    log_ratios = []
    for tc in sorted_tests:
        base_bw = all_metrics[baseline_name][tc]['actual_bandwidth_GBs']
        accel_bw = all_metrics[accel_name][tc]['actual_bandwidth_GBs']
        if base_bw > 0:
            log_ratios.append(math.log(accel_bw / base_bw))
    geomean_bw_ratio = math.exp(sum(log_ratios) / len(log_ratios)) if log_ratios else 1.0

    labels = [tc[:6] for tc in sorted_tests]

    fig_width = max(12, len(sorted_tests) * 0.5)
    fig, ax = plt.subplots(figsize=(fig_width, 6.5))

    num_folders = len(folder_names)
    bar_width = 0.8 / num_folders
    x_base = range(len(sorted_tests))

    for folder_idx, folder_name in enumerate(folder_names):
        x_positions = [x + folder_idx * bar_width for x in x_base]
        values = [all_metrics[folder_name][tc]['actual_bandwidth_GBs'] for tc in sorted_tests]

        ax.bar(x_positions, values, bar_width,
               label=folder_name,
               color=colors[folder_idx],
               alpha=0.85, edgecolor='black', linewidth=0.8)

    ax.set_ylabel('L2 Bandwidth\n(GB/s)', fontsize=fonts['axis_label'], fontweight=fonts['axis_label_weight'])
    ax.set_xticks([x + bar_width * (num_folders - 1) / 2 for x in x_base])
    ax.set_xticklabels(labels, fontsize=20, rotation=45, ha='right')
    ax.tick_params(axis='y', labelsize=fonts['tick'])
    ax.set_xlim(-0.5, len(sorted_tests) - 0.2)

    ax.grid(axis='y', alpha=0.6, linestyle='-', linewidth=1.2)
    ax.set_axisbelow(True)

    # Annotation for geometric mean
    ax.text(0.02, 0.95, f'Geomean BW ratio: {geomean_bw_ratio:.2f}x',
            transform=ax.transAxes, fontsize=fonts['annotation'],
            verticalalignment='top', bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.8))

    max_val = max(all_metrics[fn][tc]['actual_bandwidth_GBs'] for fn in folder_names for tc in sorted_tests)
    ax.set_ylim(0, max_val * 1.35)
    ax.legend(loc='upper center', fontsize=fonts['legend'], frameon=True,
              ncol=num_folders, bbox_to_anchor=(0.5, 1.02),
              handlelength=1.0, handletextpad=0.5, columnspacing=1.0)

    plt.tight_layout()
    plt.savefig(output_path, format='pdf', bbox_inches='tight')
    print(f"Bandwidth comparison chart saved to: {output_path}")
    plt.close(fig)


def plot_throughput_vs_req_per_prop(common_tests, all_metrics, folder_names, colors, fonts, output_path):
    """Plot propagations/s vs L1 requests per propagation scatter plot.

    X-axis (req/prop) is a workload characteristic measuring memory access
    intensity per propagation — largely independent of throughput.
    Points shifting left = fewer memory requests per propagation (better data reuse).
    Points shifting up = higher throughput at same req/prop (better compute efficiency).
    """
    fig, ax = plt.subplots(figsize=(12, 9))

    for folder_idx, folder_name in enumerate(folder_names):
        req_per_prop = [all_metrics[folder_name][tc]['req_per_prop'] for tc in common_tests]
        prop_per_sec = [all_metrics[folder_name][tc]['propagations_per_sec'] for tc in common_tests]

        ax.scatter(req_per_prop, prop_per_sec, color=colors[folder_idx], label=folder_name,
                   s=120, alpha=0.8, edgecolors='black', linewidth=0.5, zorder=3)

    ax.set_xscale('log')
    ax.set_yscale('log')
    ax.set_xlabel('L1 Requests/Propagation',
                  fontsize=fonts['axis_label'], fontweight=fonts['axis_label_weight'])
    ax.set_ylabel('Propagations/s',
                  fontsize=fonts['axis_label'], fontweight=fonts['axis_label_weight'])
    ax.tick_params(axis='both', labelsize=fonts['tick'])
    ax.grid(True, alpha=0.3, linestyle='--', which='both')
    ax.set_axisbelow(True)

    ax.legend(fontsize=fonts['legend'], frameon=True, loc='upper right')

    plt.tight_layout()
    plt.savefig(output_path, format='pdf', bbox_inches='tight')
    print(f"Throughput vs req/prop chart saved to: {output_path}")
    plt.close(fig)


def plot_req_per_prop_bars(common_tests, all_metrics, folder_names, colors, fonts, output_path):
    """Plot L1 requests per propagation as grouped bar chart per test."""
    # Sort by baseline req/prop (ascending)
    baseline_name = folder_names[0]
    accel_name = folder_names[1]
    sorted_tests = sorted(common_tests,
                          key=lambda tc: all_metrics[baseline_name][tc]['req_per_prop'])

    # Compute geomean req/prop ratio
    log_ratios = []
    for tc in sorted_tests:
        base_rpp = all_metrics[baseline_name][tc]['req_per_prop']
        accel_rpp = all_metrics[accel_name][tc]['req_per_prop']
        if base_rpp > 0:
            log_ratios.append(math.log(accel_rpp / base_rpp))
    geomean_rpp_ratio = math.exp(sum(log_ratios) / len(log_ratios)) if log_ratios else 1.0

    # First 5 characters of test name, slanted
    labels = [tc[:6] for tc in sorted_tests]

    fig_width = max(12, len(sorted_tests) * 0.5)
    fig, ax = plt.subplots(figsize=(fig_width, 6.5))

    num_folders = len(folder_names)
    bar_width = 0.8 / num_folders
    x_base = range(len(sorted_tests))

    for folder_idx, folder_name in enumerate(folder_names):
        x_positions = [x + folder_idx * bar_width for x in x_base]
        values = [all_metrics[folder_name][tc]['req_per_prop'] for tc in sorted_tests]

        ax.bar(x_positions, values, bar_width,
               label=folder_name,
               color=colors[folder_idx],
               alpha=0.85, edgecolor='black', linewidth=0.8)

    ax.set_yscale('log')
    ax.set_ylabel('L1 Requests /\nPropagation',
                  fontsize=fonts['axis_label'], fontweight=fonts['axis_label_weight'])
    ax.set_xticks([x + bar_width * (num_folders - 1) / 2 for x in x_base])
    ax.set_xticklabels(labels, fontsize=20, rotation=45, ha='right')
    ax.tick_params(axis='y', labelsize=fonts['tick'])
    ax.set_xlim(-0.5, len(sorted_tests) - 0.2)

    ax.grid(axis='y', alpha=0.3, linestyle='--', which='both')
    ax.set_axisbelow(True)

    # Annotation for geometric mean (invert ratio so >1x means reduction)
    geomean_rpp_reduction = 1.0 / geomean_rpp_ratio if geomean_rpp_ratio > 0 else 1.0
    ax.text(0.02, 0.95, f'Geomean req/prop\nreduction: {geomean_rpp_reduction:.2f}x',
            transform=ax.transAxes, fontsize=fonts['annotation'],
            verticalalignment='top', bbox=dict(boxstyle='round', facecolor='wheat', alpha=0.8))

    ax.legend(loc='upper center', fontsize=fonts['legend'], frameon=True,
              ncol=num_folders, bbox_to_anchor=(0.5, 1.02),
              handlelength=1.0, handletextpad=0.5, columnspacing=1.0)

    plt.tight_layout()
    plt.savefig(output_path, format='pdf', bbox_inches='tight')
    print(f"Req/prop bar chart saved to: {output_path}")
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(
        description='Compare throughput and bandwidth between two SAT solver configurations',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  %(prog)s runs/baseline runs/optimized --peak-bandwidth 16 --peak-compute 1e9
  %(prog)s logs_base logs_opt --names "Baseline" "SATBlast" --peak-bandwidth 16 --peak-compute 1e9 --output-dir results/
        """
    )

    parser.add_argument('folders', nargs=2, help='Two input folders to compare (in display order)')
    parser.add_argument('--names', nargs=2,
                        help='Custom names for each folder')
    parser.add_argument('--peak-bandwidth', type=float, required=True,
                        help='Peak memory bandwidth in GB/s')
    parser.add_argument('--peak-compute', type=float, required=True,
                        help='Peak compute throughput in propagations/s')
    parser.add_argument('--timeout', type=float, default=36,
                        help='Timeout in seconds (default: 36)')
    parser.add_argument('--output-dir', default='results',
                        help='Output directory for plots (default: results/)')
    args = parser.parse_args()
    timeout_ms = args.timeout * 1000.0
    fonts = get_font_sizes()
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"Comparing throughput and bandwidth between 2 folders\n")

    # Parse results
    folder_names = []
    all_metrics = {}

    for i, folder_path in enumerate(args.folders):
        folder_name = args.names[i] if args.names else Path(folder_path).name
        print(f"Processing {folder_name} ({folder_path})...")

        results = parse_log_directory(Path(folder_path), exclude_summary=True)
        if not results:
            print(f"  Error: No valid data found in {folder_path}")
            sys.exit(1)

        metrics = compute_metrics(results, timeout_ms)
        print(f"  Tests with valid data: {len(metrics)}")

        folder_names.append(folder_name)
        all_metrics[folder_name] = metrics

    # Find common tests across both folders (excluding manual exclusions)
    common_tests = set(all_metrics[folder_names[0]].keys()) & set(all_metrics[folder_names[1]].keys())
    common_tests -= MANUAL_EXCLUSIONS
    common_tests = sorted(common_tests)

    if not common_tests:
        print("\nError: No common tests with valid L1 cache data found")
        sys.exit(1)

    print(f"\nCommon tests with valid data: {len(common_tests)}")

    # Get colors
    colors = get_folder_colors(folder_names)

    # Generate plots
    plot_propagations_per_sec(
        common_tests, all_metrics, folder_names, colors, fonts,
        output_dir / 'propagations_per_sec.pdf')

    plot_roofline(
        common_tests, all_metrics, folder_names, colors, fonts,
        args.peak_bandwidth, args.peak_compute,
        output_dir / 'roofline.pdf')

    plot_bandwidth_comparison(
        common_tests, all_metrics, folder_names, colors, fonts,
        output_dir / 'bandwidth_comparison.pdf')

    plot_throughput_vs_req_per_prop(
        common_tests, all_metrics, folder_names, colors, fonts,
        output_dir / 'throughput_vs_req_per_prop.pdf')

    plot_req_per_prop_bars(
        common_tests, all_metrics, folder_names, colors, fonts,
        output_dir / 'req_per_prop.pdf')

    # Print summary
    print(f"\n{'='*60}")
    print(f"Summary ({len(common_tests)} benchmarks)")
    print(f"{'='*60}")

    for folder_name in folder_names:
        ps_vals = [all_metrics[folder_name][tc]['propagations_per_sec'] for tc in common_tests]
        bw_vals = [all_metrics[folder_name][tc]['actual_bandwidth_GBs'] for tc in common_tests]
        oi_vals = [all_metrics[folder_name][tc]['operational_intensity'] for tc in common_tests]
        rpp_vals = [all_metrics[folder_name][tc]['req_per_prop'] for tc in common_tests]
        avg_ps = sum(ps_vals) / len(ps_vals)
        max_ps = max(ps_vals)
        avg_bw = sum(bw_vals) / len(bw_vals)
        avg_oi = sum(oi_vals) / len(oi_vals)
        avg_rpp = sum(rpp_vals) / len(rpp_vals)
        median_rpp = sorted(rpp_vals)[len(rpp_vals) // 2]
        print(f"\n{folder_name}:")
        print(f"  Avg propagations/s:       {avg_ps:.2e}")
        print(f"  Max propagations/s:       {max_ps:.2e}  (empirical peak compute)")
        print(f"  Avg L2 bandwidth (GB/s):     {avg_bw:.4f}")
        print(f"  Avg operational intensity: {avg_oi:.4f} prop/byte")
        print(f"  Avg L1 req/propagation:   {avg_rpp:.1f}")
        print(f"  Median L1 req/propagation: {median_rpp:.1f}")


if __name__ == "__main__":
    main()
