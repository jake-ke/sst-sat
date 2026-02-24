#!/usr/bin/env python3
"""
2WL Clause Access Reduction Plotter

Plots the effectiveness of the 2-Watched-Literal (2WL) scheme in reducing
clause accesses compared to a naive full occurrence list approach.

Panel 1: Reduction % vs number of clauses (problem size)
Panel 2: Reduction % vs number of propagations (solver activity)

Usage: python plot_2wl_reduction.py <logs_dir> [output.pdf]
"""

import sys
from pathlib import Path
import matplotlib.pyplot as plt
import numpy as np
from unified_parser import parse_log_directory


def collect_2wl_data(logs_dir):
    """Parse logs and extract 2WL reduction data with problem characteristics.

    Returns list of dicts with keys: test_case, clauses, variables,
    propagations, twl_reduction_pct, twl_naive_accesses,
    twl_watchers_traversed, result.
    """
    results = parse_log_directory(logs_dir, exclude_summary=True)
    finished = [r for r in results if r.get('result') not in ('ERROR', 'UNKNOWN')]

    data = []
    for r in finished:
        pct = r.get('twl_reduction_pct')
        if pct is None:
            continue
        data.append({
            'test_case': r.get('test_case', ''),
            'clauses': r.get('clauses', 0),
            'variables': r.get('variables', 0),
            'propagations': r.get('propagations', 0),
            'conflicts': r.get('conflicts', 0),
            'twl_reduction_pct': pct,
            'twl_naive_accesses': r.get('twl_naive_accesses', 0),
            'twl_watchers_traversed': r.get('twl_watchers_traversed', 0),
            'result': r.get('result', ''),
        })

    return data


def print_summary(data):
    """Print summary statistics to stdout."""
    if not data:
        print("No data to summarize.")
        return

    pcts = [d['twl_reduction_pct'] for d in data]
    clauses = [d['clauses'] for d in data]
    props = [d['propagations'] for d in data]

    print(f"\n{'='*60}")
    print(f"  2WL Clause Access Reduction Summary  ({len(data)} tests)")
    print(f"{'='*60}")
    print(f"  Reduction %:  min={min(pcts):.1f}%  max={max(pcts):.1f}%  "
          f"mean={np.mean(pcts):.1f}%  median={np.median(pcts):.1f}%")
    print(f"  Clauses:      min={min(clauses)}  max={max(clauses)}  "
          f"median={int(np.median(clauses))}")
    print(f"  Propagations: min={min(props)}  max={max(props)}  "
          f"median={int(np.median(props))}")

    total_naive = sum(d['twl_naive_accesses'] for d in data)
    total_traversed = sum(d['twl_watchers_traversed'] for d in data)
    overall_pct = (1.0 - total_traversed / total_naive) * 100.0 if total_naive > 0 else 0
    print(f"\n  Aggregate:    {total_naive:,} naive accesses -> "
          f"{total_traversed:,} watchers traversed")
    print(f"                Overall reduction: {overall_pct:.1f}%")
    print(f"{'='*60}\n")


def plot_2wl_reduction(data, output_pdf):
    """Create a 2-panel scatter plot of 2WL reduction effectiveness."""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))

    # Separate by result type
    sat_data = [d for d in data if d['result'] == 'SAT']
    unsat_data = [d for d in data if d['result'] == 'UNSAT']
    timeout_data = [d for d in data if d['result'] == 'TIMEOUT']

    def scatter_by_result(ax, x_key, groups, xlabel):
        for label, subset, color, marker in groups:
            if not subset:
                continue
            x = [d[x_key] for d in subset]
            y = [d['twl_reduction_pct'] for d in subset]
            ax.scatter(x, y, c=color, marker=marker, s=40, alpha=0.7,
                       edgecolors='black', linewidths=0.3, label=label, zorder=5)

        # Trend line across all data
        all_x = np.array([d[x_key] for d in data], dtype=float)
        all_y = np.array([d['twl_reduction_pct'] for d in data], dtype=float)
        mask = all_x > 0
        if mask.sum() > 2:
            log_x = np.log10(all_x[mask])
            z = np.polyfit(log_x, all_y[mask], 1)
            p = np.poly1d(z)
            x_fit = np.linspace(log_x.min(), log_x.max(), 100)
            ax.plot(10**x_fit, p(x_fit), '--', color='gray', linewidth=1.5,
                    alpha=0.6, label='Trend', zorder=4)

        ax.set_xlabel(xlabel, fontsize=14, fontweight='bold')
        ax.set_ylabel('Clause Access Reduction (%)', fontsize=14, fontweight='bold')
        ax.set_xscale('log')
        ax.grid(True, alpha=0.3, linestyle='--')
        ax.tick_params(axis='both', which='major', labelsize=12)
        ax.legend(fontsize=11)

    groups = [
        ('SAT', sat_data, '#4C72B0', 'o'),
        ('UNSAT', unsat_data, '#DD8452', 's'),
        ('TIMEOUT', timeout_data, '#C44E52', '^'),
    ]

    scatter_by_result(ax1, 'clauses', groups, 'Number of Clauses')
    scatter_by_result(ax2, 'propagations', groups, 'Number of Propagations')

    ax1.set_title('Clause Access Reductions Over Clauses', fontsize=14)
    ax2.set_title('Clause Access Reductions Over Propagations', fontsize=14)

    plt.tight_layout()
    plt.savefig(output_pdf, format='pdf', bbox_inches='tight', dpi=300)
    print(f"Plot saved to: {output_pdf}")
    plt.close()


def main():
    if len(sys.argv) < 2:
        print("Usage: python plot_2wl_reduction.py <logs_dir> [output.pdf]")
        sys.exit(1)

    logs_dir = sys.argv[1]
    output_pdf = sys.argv[2] if len(sys.argv) > 2 else '2wl_reduction.pdf'

    print(f"Parsing logs from: {logs_dir}")
    data = collect_2wl_data(logs_dir)

    if not data:
        print("Error: No valid 2WL reduction data found.")
        sys.exit(1)

    print_summary(data)
    plot_2wl_reduction(data, output_pdf)


if __name__ == "__main__":
    main()
