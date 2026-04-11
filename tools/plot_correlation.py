#!/usr/bin/env python3
"""
Simulation Validation: baseline SST simulator vs MiniSat on real hardware.

Generates a 2-row figure:
  Top row:  Algorithmic validation (solver logic correctness)
  Bottom row: Runtime comparison (expected divergence due to different hw configs)

Usage:
    python3 tools/plot_correlation.py <baseline_dir> <minisat_dir> \
        [--timeout 36] [--output correlation.pdf]
"""

import sys
import argparse
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D
import numpy as np

from unified_parser import parse_log_directory


MANUAL_EXCLUSIONS = {
    "080896c437245ac25eb6d3ad6df12c4f-bv-term-small-rw_1492.smt2.cnf",
    "e17d3f94f2c0e11ce6143bc4bf298bd7-mp1-qpr-bmp280-driver-5.cnf",
    "e185ebbd92c23ab460a3d29046eccf1d-group_mulr.cnf",
    # MiniSat preprocessing solves this before search (0 decisions) — unfair comparison
    "17039a3ed02ea12653ec5389e56dab50-pbl-00070.shuffled-as.sat05-1324.shuffled-as.sat05-1324.cnf",
}

COLOR_SAT = '#1f77b4'
COLOR_UNSAT = '#d62728'
COLOR_BASE = '#d62728'
COLOR_MINI = '#1f77b4'
COLOR_ALGO = '#2ca02c'
COLOR_RT = '#ff7f0e'

# Only the 4 metrics we care about
ALGO_METRICS = [
    ('decisions', 'Decisions'),
    ('propagations', 'Propagations'),
    ('conflicts', 'Conflicts'),
]


def normalize_test_case(name):
    if not name:
        return name
    known_exts = {'.cnf', '.dimacs', '.smt2', '.xml', '.c', '.txt', '.log'}
    lower = name.lower()
    for ext in known_exts:
        if lower.endswith(ext):
            return name[:-(len(ext))]
    return name


def pearson_r(x, y):
    if len(x) < 2:
        return 0.0
    xm = x - np.mean(x)
    ym = y - np.mean(y)
    denom = np.sqrt(np.sum(xm**2) * np.sum(ym**2))
    if denom == 0:
        return 0.0
    return float(np.sum(xm * ym) / denom)


def spearman_rho(x, y):
    if len(x) < 2:
        return 0.0
    def rankdata(arr):
        order = np.argsort(arr)
        ranks = np.empty_like(order, dtype=float)
        ranks[order] = np.arange(1, len(arr) + 1, dtype=float)
        for val in np.unique(arr):
            mask = arr == val
            ranks[mask] = ranks[mask].mean()
        return ranks
    return pearson_r(rankdata(x), rankdata(y))


def load_data(directory):
    raw = parse_log_directory(directory, exclude_summary=True)
    if not raw:
        print(f"Warning: no results from {directory}")
        return {}
    data = {}
    for r in raw:
        name = normalize_test_case(r.get('test_case', ''))
        if name:
            data[name] = r
    return data


def match_cases(base_data, mini_data, timeout_ms):
    common = set(base_data.keys()) & set(mini_data.keys())
    excl_norm = {normalize_test_case(e) for e in MANUAL_EXCLUSIONS}
    common -= excl_norm
    matched = []
    for name in sorted(common):
        b = base_data[name]
        m = mini_data[name]
        br = b.get('result', 'UNKNOWN')
        mr = m.get('result', 'UNKNOWN')
        if br in ('TIMEOUT', 'ERROR', 'UNKNOWN') or mr in ('TIMEOUT', 'ERROR', 'UNKNOWN', 'INDETERMINATE'):
            continue
        b_ms = float(b.get('sim_time_ms', 0) or 0)
        m_ms = float(m.get('sim_time_ms', 0) or 0)
        if b_ms > timeout_ms or m_ms > timeout_ms:
            continue
        matched.append((name, b, m))
    return matched


def get_metric(entries, key):
    bvals, mvals, results = [], [], []
    for name, b, m in entries:
        bv = float(b.get(key, 0) or 0)
        mv = float(m.get(key, 0) or 0)
        bvals.append(bv)
        mvals.append(mv)
        results.append(b.get('result', 'UNKNOWN'))
    return np.array(bvals), np.array(mvals), results


# ═══════════════════════════════════════════════════════════════════════════════
# Panel (a): Scatter grid — Decisions, Propagations, Conflicts side by side
# ═══════════════════════════════════════════════════════════════════════════════
def plot_algo_scatter(ax, x, y, results, label, annotate_rho=True):
    """Log-log scatter for one algorithmic metric."""
    colors = [COLOR_SAT if r == 'SAT' else COLOR_UNSAT for r in results]
    xp = np.maximum(x, 0.5)
    yp = np.maximum(y, 0.5)

    ax.scatter(xp, yp, c=colors, alpha=0.6, s=35, edgecolors='none', zorder=2)
    ax.set_xscale('log')
    ax.set_yscale('log')

    # y=x line
    lo = min(xp.min(), yp.min()) * 0.3
    hi = max(xp.max(), yp.max()) * 3
    ax.plot([lo, hi], [lo, hi], 'k--', alpha=0.3, linewidth=1, zorder=1)

    # Correlation stats
    mask = (x > 0) & (y > 0)
    rho = spearman_rho(x[mask], y[mask])
    r = pearson_r(np.log10(xp[mask]), np.log10(yp[mask]))
    r2 = r ** 2
    n = mask.sum()

    if annotate_rho:
        textstr = f'$\\rho$ = {rho:.2f}\n$R^2$ = {r2:.2f}\nn = {n}'
        ax.text(0.05, 0.95, textstr, transform=ax.transAxes, fontsize=12,
                verticalalignment='top',
                bbox=dict(boxstyle='round', facecolor='#e8f5e9', alpha=0.9, edgecolor=COLOR_ALGO))

    ax.set_xlabel(f'Simulator {label}', fontsize=13)
    ax.set_ylabel(f'MiniSat {label}', fontsize=13)
    ax.set_title(label, fontsize=15, fontweight='bold')
    ax.grid(True, alpha=0.3, linestyle='--')


# ═══════════════════════════════════════════════════════════════════════════════
# Panel (b): Runtime scatter — showing expected divergence
# ═══════════════════════════════════════════════════════════════════════════════
def plot_runtime_scatter(ax, matched):
    """Runtime scatter with annotation explaining the gap."""
    bx, mx, results = get_metric(matched, 'sim_time_ms')
    colors = [COLOR_SAT if r == 'SAT' else COLOR_UNSAT for r in results]
    xp = np.maximum(bx, 0.5)
    yp = np.maximum(mx, 0.5)

    ax.scatter(xp, yp, c=colors, alpha=0.6, s=35, edgecolors='none', zorder=2)
    ax.set_xscale('log')
    ax.set_yscale('log')

    # y=x line
    lo = min(xp.min(), yp.min()) * 0.3
    hi = max(xp.max(), yp.max()) * 3
    ax.plot([lo, hi], [lo, hi], 'k--', alpha=0.3, linewidth=1, zorder=1)

    # Best-fit line in log space
    lx = np.log10(xp)
    ly = np.log10(yp)
    coeffs = np.polyfit(lx, ly, 1)
    fit_x = np.linspace(lx.min(), lx.max(), 100)
    fit_y = np.polyval(coeffs, fit_x)
    ax.plot(10**fit_x, 10**fit_y, color=COLOR_RT, linewidth=2, alpha=0.7,
            linestyle='-', zorder=1, label=f'fit (slope={coeffs[0]:.2f})')

    mask = (bx > 0) & (mx > 0)
    rho = spearman_rho(bx[mask], mx[mask])
    r = pearson_r(np.log10(xp[mask]), np.log10(yp[mask]))
    r2 = r ** 2
    median_ratio = np.median(mx[mask] / bx[mask])

    textstr = (f'$\\rho$ = {rho:.2f}\n'
               f'$R^2$ = {r2:.2f}\n'
               f'n = {mask.sum()}')
    ax.text(0.05, 0.95, textstr, transform=ax.transAxes, fontsize=12,
            verticalalignment='top',
            bbox=dict(boxstyle='round', facecolor='#fff3e0', alpha=0.9, edgecolor=COLOR_RT))

    ax.set_xlabel('Simulator Runtime (ms)\n[1 GHz, 128KB L1, 24MB L2]', fontsize=12)
    ax.set_ylabel('MiniSat Runtime (ms)\n[5.8 GHz i9-13900K]', fontsize=12)
    ax.set_title('Runtime', fontsize=15, fontweight='bold')
    ax.grid(True, alpha=0.3, linestyle='--')
    ax.legend(fontsize=10, loc='lower right')


# ═══════════════════════════════════════════════════════════════════════════════
# Panel (c): Summary bar — all metrics ρ side by side, colored by category
# ═══════════════════════════════════════════════════════════════════════════════
def plot_summary_bars(ax, matched):
    """Grouped horizontal bars: Spearman ρ and Pearson R² side by side."""
    items = [
        ('decisions', 'Decisions', 'algo'),
        ('propagations', 'Propagations', 'algo'),
        ('conflicts', 'Conflicts', 'algo'),
        ('sim_time_ms', 'Runtime', 'runtime'),
    ]

    labels, rhos, r2s, categories = [], [], [], []
    for key, label, cat in items:
        bx, mx, _ = get_metric(matched, key)
        mask = (bx > 0) & (mx > 0)
        if mask.sum() < 3:
            continue
        rho = spearman_rho(bx[mask], mx[mask])
        r = pearson_r(np.log10(np.maximum(bx[mask], 0.5)),
                      np.log10(np.maximum(mx[mask], 0.5)))
        labels.append(label)
        rhos.append(rho)
        r2s.append(r ** 2)
        categories.append(cat)

    y_pos = np.arange(len(labels))
    bar_h = 0.35

    # ρ bars (top of each row)
    colors_rho = [COLOR_ALGO if c == 'algo' else COLOR_RT for c in categories]
    bars_rho = ax.barh(y_pos - bar_h/2, rhos, color=colors_rho, alpha=0.8,
                       height=bar_h, edgecolor='white', linewidth=0.5, label='Spearman $\\rho$')

    # R² bars (bottom of each row)
    colors_r2 = [COLOR_ALGO if c == 'algo' else COLOR_RT for c in categories]
    bars_r2 = ax.barh(y_pos + bar_h/2, r2s, color=colors_r2, alpha=0.4,
                      height=bar_h, edgecolor='white', linewidth=0.5,
                      hatch='///', label='Pearson $R^2$ (log-log)')

    for i in range(len(labels)):
        ax.text(rhos[i] + 0.02, y_pos[i] - bar_h/2, f'{rhos[i]:.3f}',
                va='center', fontsize=11, fontweight='bold')
        ax.text(r2s[i] + 0.02, y_pos[i] + bar_h/2, f'{r2s[i]:.3f}',
                va='center', fontsize=11, fontweight='bold')

    ax.set_xlim(0, 1.15)
    ax.set_yticks(y_pos)
    ax.set_yticklabels(labels, fontsize=13)
    ax.set_xlabel('Correlation', fontsize=14)
    ax.set_title('Correlation Summary', fontsize=15, fontweight='bold')
    ax.grid(True, alpha=0.3, linestyle='--', axis='x')
    ax.invert_yaxis()

    ax.legend(fontsize=10, loc='lower right', frameon=True)


# ═══════════════════════════════════════════════════════════════════════════════
# Panel (d): Agreement table
# ═══════════════════════════════════════════════════════════════════════════════
def plot_agreement_table(ax, matched):
    """Compact table: result agreement + metric agreement percentages."""
    ax.axis('off')

    same_result = sum(1 for _, b, m in matched if b.get('result') == m.get('result'))
    total = len(matched)

    all_items = [
        ('decisions', 'Decisions'),
        ('propagations', 'Propagations'),
        ('conflicts', 'Conflicts'),
        ('sim_time_ms', 'Runtime'),
    ]

    rows = []
    for key, label in all_items:
        bx, mx, _ = get_metric(matched, key)
        mask = (bx > 0) & (mx > 0)
        n = mask.sum()
        if n == 0:
            continue
        rho = spearman_rho(bx[mask], mx[mask])
        r = pearson_r(np.log10(np.maximum(bx[mask], 0.5)),
                      np.log10(np.maximum(mx[mask], 0.5)))
        r2 = r ** 2
        is_algo = key != 'sim_time_ms'
        rows.append((label, n, rho, r2, is_algo))

    # Layout
    col_x = [0.02, 0.28, 0.42, 0.62, 0.82]
    header = ['Metric', 'n', '\u03c1', 'R\u00b2 (log)', 'Agreement']
    y = 0.92
    row_h = 0.10

    ax.text(0.5, 1.0, f'Simulation Validation (n={total})',
            transform=ax.transAxes, fontsize=15, fontweight='bold',
            ha='center', va='top')

    ax.text(0.5, y,
            f'SAT/UNSAT agreement: {same_result}/{total} ({100*same_result/total:.0f}%)',
            transform=ax.transAxes, fontsize=13, ha='center', va='top',
            color=COLOR_ALGO if same_result == total else COLOR_UNSAT,
            fontweight='bold')

    y -= 1.5 * row_h

    # Header
    for j, h in enumerate(header):
        ax.text(col_x[j], y, h, transform=ax.transAxes, fontsize=12,
                fontweight='bold', va='center')
    y -= 0.05
    ax.plot([0.02, 0.98], [y + 0.01, y + 0.01], color='k', linewidth=0.8,
            transform=ax.transAxes, clip_on=False)

    # Separator before runtime
    runtime_sep_drawn = False

    for label, n, rho, r2, is_algo in rows:
        if not is_algo and not runtime_sep_drawn:
            y -= 0.03
            ax.plot([0.02, 0.98], [y + 0.01, y + 0.01], color='#999999',
                    linewidth=0.5, linestyle='--', transform=ax.transAxes, clip_on=False)
            runtime_sep_drawn = True

        y -= row_h
        row_color = 'black' if is_algo else '#666666'

        # Agreement label
        if rho >= 0.8 and r2 >= 0.7:
            agree = 'Strong'
            agree_color = COLOR_ALGO
        elif rho >= 0.7 and r2 >= 0.5:
            agree = 'Good'
            agree_color = COLOR_ALGO
        elif rho >= 0.5:
            agree = 'Moderate'
            agree_color = COLOR_RT
        else:
            agree = 'Weak'
            agree_color = COLOR_UNSAT

        vals = [label, str(n), f'{rho:.3f}', f'{r2:.3f}', agree]
        val_colors = [row_color, row_color,
                      COLOR_ALGO if rho >= 0.7 else COLOR_RT,
                      COLOR_ALGO if r2 >= 0.5 else COLOR_RT,
                      agree_color]
        for j, (v, c) in enumerate(zip(vals, val_colors)):
            ax.text(col_x[j], y, v, transform=ax.transAxes, fontsize=11,
                    va='center', color=c,
                    fontweight='bold' if j >= 4 else 'normal',
                    fontstyle='italic' if not is_algo else 'normal')

    # Footnote
    y -= 1.8 * row_h
    ax.text(0.5, y,
            'Algorithmic metrics validate solver correctness.\n'
            'Runtime diverges due to different hardware configurations\n'
            '(simulator: 1 GHz, 128KB L1; MiniSat: 5.8 GHz i9-13900K).',
            transform=ax.transAxes, fontsize=9, ha='center', va='top',
            color='#666666', style='italic')


import matplotlib.patches as mpatches


def main():
    parser = argparse.ArgumentParser(description='Simulation validation: baseline vs MiniSat')
    parser.add_argument('baseline_dir', help='Baseline simulator log directory')
    parser.add_argument('minisat_dir', help='MiniSat log directory')
    parser.add_argument('--timeout', type=float, default=36, help='Timeout in seconds (default: 36)')
    parser.add_argument('--output', default='correlation.pdf', help='Output file (default: correlation.pdf)')
    args = parser.parse_args()

    timeout_ms = args.timeout * 1000.0

    print(f"Loading baseline from {args.baseline_dir} ...")
    base_data = load_data(args.baseline_dir)
    print(f"  Found {len(base_data)} test cases")

    print(f"Loading MiniSat from {args.minisat_dir} ...")
    mini_data = load_data(args.minisat_dir)
    print(f"  Found {len(mini_data)} test cases")

    matched = match_cases(base_data, mini_data, timeout_ms)
    print(f"Matched {len(matched)} test cases (after filtering timeouts/errors)")

    if not matched:
        print("ERROR: No matched test cases found.")
        sys.exit(1)

    # ── 2×3 layout: top = algorithmic, bottom = runtime + summary ────────────
    fig = plt.figure(figsize=(20, 12))

    # Top row: 3 algorithmic scatter plots
    ax_dec = fig.add_subplot(2, 3, 1)
    ax_prop = fig.add_subplot(2, 3, 2)
    ax_conf = fig.add_subplot(2, 3, 3)

    # Bottom row: runtime scatter, summary bars, agreement table
    ax_rt = fig.add_subplot(2, 3, 4)
    ax_bars = fig.add_subplot(2, 3, 5)
    ax_table = fig.add_subplot(2, 3, 6)

    # ── Top row: Algorithmic Validation ──────────────────────────────────────
    for ax_i, (key, label) in zip([ax_dec, ax_prop, ax_conf], ALGO_METRICS):
        bx, mx, res = get_metric(matched, key)
        plot_algo_scatter(ax_i, bx, mx, res, label)

    # ── Bottom row: Runtime + Summary ────────────────────────────────────────
    plot_runtime_scatter(ax_rt, matched)
    plot_summary_bars(ax_bars, matched)
    plot_agreement_table(ax_table, matched)

    # ── Row labels ───────────────────────────────────────────────────────────
    fig.text(0.01, 0.75, 'Algorithmic\nValidation',
             fontsize=14, fontweight='bold', color=COLOR_ALGO,
             ha='left', va='center', rotation=90)
    fig.text(0.01, 0.28, 'Runtime\nComparison',
             fontsize=14, fontweight='bold', color=COLOR_RT,
             ha='left', va='center', rotation=90)

    # ── Shared legend ────────────────────────────────────────────────────────
    legend_elements = [
        Line2D([0], [0], marker='o', color='w', markerfacecolor=COLOR_SAT,
               markersize=8, label='SAT'),
        Line2D([0], [0], marker='o', color='w', markerfacecolor=COLOR_UNSAT,
               markersize=8, label='UNSAT'),
        Line2D([0], [0], linestyle='--', color='k', alpha=0.3, label='y = x'),
    ]
    fig.legend(handles=legend_elements, loc='lower center', ncol=3, fontsize=12,
               frameon=True, bbox_to_anchor=(0.35, 0.005))

    fig.suptitle('Simulation Validation: Baseline Simulator vs MiniSat on Real Hardware'
                 f'  (n={len(matched)})',
                 fontsize=18, fontweight='bold', y=0.98)
    fig.tight_layout(rect=[0.03, 0.03, 1, 0.96])

    fig.savefig(args.output, format='pdf', dpi=300, bbox_inches='tight')
    print(f"Saved to {args.output}")


if __name__ == '__main__':
    main()
