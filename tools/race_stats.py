#!/usr/bin/env python3
"""Extract and visualize propagation synchronization-sizing statistics.

Reads the per-instance SST statistics CSVs in a run folder produced by
run_sc_l2.sh (layout: <run_folder>/seed*/<case>.stats.csv) and summarizes the
SATSolver propagation-lock sizing stats:

  Conflict episodes (counts):
    clause_conflicts      - stalls on an already-locked clause
    wl_insert_conflicts   - stalls on a busy watchlist (insertion)
    wl_process_conflicts  - stalls on pending watchlist insertions (processing)
  Occupancy distributions (histograms):
    clause_lock_occ  - concurrent locked clauses     (sizes the clause-lock CAM)
    busy_occ         - concurrent watchlist inserts   (sizes write-ports)
    wl_q_occ         - pending watchlist insertions   (sizes the insertion queue)
    blocked_workers  - workers blocked on a lock per scheduler round

Outputs a per-instance CSV summary plus three PDF charts:
  - race_occupancy_distributions.pdf  aggregate occupancy distributions
  - race_conflicts.pdf                four views of conflict rate by lock type
  - race_hwm_summary.pdf              per-instance high-water-mark distributions

Requires --enable-histograms to have been set during the run (the occupancy
stats are histograms; the conflict counters are always emitted).

Usage:
  python tools/race_stats.py <run_folder> [--out-dir DIR] [--csv FILE]
"""

import sys
import csv
import argparse
from pathlib import Path
from collections import defaultdict

import random
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

# Import the shared sync-stats parser from the sibling module.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from unified_parser import (parse_sync_stats_full, hist_scalars,
                            SYNC_CONFLICT_STATS, SYNC_OCC_STATS)
# Shared exclusion list (drops the did-not-finish tests) — single source of truth.
from plot_comparison import MANUAL_EXCLUSIONS

OCC_LABELS = {
    'clause_lock_occ': 'Clause-lock occupancy\n(concurrent locked clauses)',
    'busy_occ': 'Watchlist write-lock occupancy\n(concurrent insertions)',
    'wl_q_occ': 'Pending watchlist insertions\n(insertion-queue depth)',
    'blocked_workers': 'Blocked workers per scheduler round',
}
CONFLICT_LABELS = {
    'clause_conflicts': 'clause',
    'wl_insert_conflicts': 'wl-insert',
    'wl_process_conflicts': 'wl-process',
}
CONFLICT_COLORS = {
    'clause_conflicts': '#4C72B0',
    'wl_insert_conflicts': '#DD8452',
    'wl_process_conflicts': '#55A868',
}
# Total parallel workers = PARA_LITS * PROPAGATORS (src/structs.h). Used to turn
# the mean blocked-worker count into a fraction of parallel capacity lost.
N_WORKERS = 56


def prettify(case):
    """Shorten an instance filename for plotting (drop md5 prefix + extension)."""
    name = case
    if len(name) > 33 and name[32] == '-' and all(c in '0123456789abcdef' for c in name[:32].lower()):
        name = name[33:]
    for ext in ('.cnf', '.dimacs', '.txt'):
        if name.endswith(ext):
            name = name[:-len(ext)]
            break
    return name if len(name) <= 40 else name[:37] + '...'


def discover_stats_csvs(run_folder):
    """Return {case_name: [stats_csv_path, ...]} across seed* subdirs (or flat)."""
    run_folder = Path(run_folder)
    seed_dirs = sorted([d for d in run_folder.glob('seed*') if d.is_dir()])
    search_dirs = seed_dirs if seed_dirs else [run_folder]
    cases = defaultdict(list)
    for d in search_dirs:
        for sc in sorted(d.glob('*.stats.csv')):
            case = sc.name[:-len('.stats.csv')]
            cases[case].append(sc)
    return cases


def read_propagations(stats_csv):
    """Read solver 'propagations' (final cumulative Sum.u64) — the denominator
    for the watchlist-process conflict rate (one wait per propagateLiteral).

    With rate:"1s" the accumulator is dumped repeatedly; keep the LAST row,
    which is the cumulative total at end of simulation.
    """
    val = 0
    try:
        with open(stats_csv, newline='') as f:
            for row in csv.DictReader(f):
                if (row.get('ComponentName') == 'solver'
                        and row.get('StatisticName') == 'propagations'):
                    val = int(row.get('Sum.u64') or 0)
    except Exception:
        pass
    return val


def read_sim_time_ms(stats_csv):
    """Final simulated time in milliseconds (max SimTime column, ps -> ms)."""
    tmax = 0.0
    try:
        with open(stats_csv, newline='') as f:
            for row in csv.DictReader(f):
                try:
                    t = float(row.get('SimTime') or 0)
                except (TypeError, ValueError):
                    continue
                if t > tmax:
                    tmax = t
    except Exception:
        pass
    return tmax / 1e9


def read_stalled_per_cycle(stats_csv):
    """Return (Sum, Count) for the stalled_per_cycle accumulator (final row).
    Mean = Sum/Count = time-weighted average blocked workers per cycle."""
    s = c = 0
    try:
        with open(stats_csv, newline='') as f:
            for row in csv.DictReader(f):
                if (row.get('ComponentName') == 'solver'
                        and row.get('StatisticName') == 'stalled_per_cycle'):
                    s = int(row.get('Sum.u64') or 0)
                    c = int(row.get('Count.u64') or 0)
    except Exception:
        pass
    return s, c


def aggregate_case(stats_csvs):
    """Merge sync stats across one case's seed CSVs.

    Sums conflict counts and histogram bins across seeds (HWM falls out as the
    max occurring value). Returns None if no sync data is present.
    """
    conflicts = defaultdict(int)
    merged = {name: {'bins': defaultdict(int), 'items': 0, 'overflow': 0}
              for name in SYNC_OCC_STATS}
    props = 0
    stalled_sum = stalled_cnt = 0
    found = False
    for sc in stats_csvs:
        full = parse_sync_stats_full(sc)
        for k, v in full['conflicts'].items():
            conflicts[k] += v
            found = True
        for name, hist in full['histograms'].items():
            found = True
            for val, cnt in hist['bins'].items():
                merged[name]['bins'][val] += cnt
            merged[name]['items'] += hist['items']
            merged[name]['overflow'] += hist['overflow']
        props += read_propagations(sc)
        s, c = read_stalled_per_cycle(sc)
        stalled_sum += s
        stalled_cnt += c
    if not found:
        return None
    out = {'conflicts': dict(conflicts), 'histograms': {}, 'propagations': props,
           'stalled_sum': stalled_sum, 'stalled_cnt': stalled_cnt}
    for name, h in merged.items():
        if h['bins']:
            out['histograms'][name] = {'bins': dict(h['bins']),
                                       'items': h['items'], 'overflow': h['overflow']}
    return out


def build_records(cases):
    """Build one summary record per instance that has sync data."""
    records = []
    for case in sorted(cases):
        agg = aggregate_case(cases[case])
        if agg is None:
            continue
        rec = {'test_case': case, 'pretty': prettify(case)}
        for name in SYNC_CONFLICT_STATS:
            rec[name] = agg['conflicts'].get(name, 0)
        for name in SYNC_OCC_STATS:
            h = agg['histograms'].get(name)
            if h:
                hwm, mean, busy = hist_scalars(h)
                rec[f'{name}_hwm'] = hwm
                rec[f'{name}_mean'] = round(mean, 3)
                rec[f'{name}_bins'] = h['bins']
            else:
                rec[f'{name}_hwm'] = 0
                rec[f'{name}_mean'] = 0.0
                rec[f'{name}_bins'] = {}
            if name == 'blocked_workers':
                rec['blocked_workers_busy_frac'] = round(
                    hist_scalars(h)[2] if h else 0.0, 4)

        # Conflict RATE = stall episodes / acquisitions (run-length-normalized).
        # Denominators: clause-lock acquisitions and watchlist inserts are the
        # histogram sample counts; literal propagations for wl-process.
        rec['sim_time_ms'] = max((read_sim_time_ms(sc) for sc in cases[case]), default=0.0)
        rec['clause_acq'] = agg['histograms'].get('clause_lock_occ', {}).get('items', 0)
        rec['insert_acq'] = agg['histograms'].get('busy_occ', {}).get('items', 0)
        rec['props'] = agg.get('propagations', 0)
        rec['clause_rate'] = round(100 * rec['clause_conflicts'] / rec['clause_acq'], 4) if rec['clause_acq'] else 0.0
        rec['wl_insert_rate'] = round(100 * rec['wl_insert_conflicts'] / rec['insert_acq'], 4) if rec['insert_acq'] else 0.0
        rec['wl_process_rate'] = round(100 * rec['wl_process_conflicts'] / rec['props'], 5) if rec['props'] else 0.0
        # Time-weighted average blocked workers per cycle (and as % of N_WORKERS).
        rec['avg_stalled_per_cycle'] = round(agg['stalled_sum'] / agg['stalled_cnt'], 5) if agg.get('stalled_cnt') else 0.0
        rec['capacity_lost_pct'] = round(100 * rec['avg_stalled_per_cycle'] / N_WORKERS, 4)
        records.append(rec)
    return records


def write_csv(records, out_csv):
    fields = ['test_case', 'sim_time_ms'] + list(SYNC_CONFLICT_STATS)
    fields += ['clause_rate', 'wl_insert_rate', 'wl_process_rate']
    for name in SYNC_OCC_STATS:
        fields += [f'{name}_hwm', f'{name}_mean']
    fields += ['blocked_workers_busy_frac', 'avg_stalled_per_cycle', 'capacity_lost_pct']
    with open(out_csv, 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=fields, extrasaction='ignore')
        w.writeheader()
        for r in sorted(records, key=lambda x: x['test_case']):
            w.writerow(r)


def plot_occupancy_distributions(records, out_pdf):
    """Aggregate occupancy distribution (summed over instances) per structure."""
    fig, axes = plt.subplots(2, 2, figsize=(12, 8))
    for ax, name in zip(axes.flat, SYNC_OCC_STATS):
        agg = defaultdict(int)
        for r in records:
            for val, cnt in r.get(f'{name}_bins', {}).items():
                agg[val] += cnt
        if not agg:
            ax.set_title(OCC_LABELS[name] + '\n(no data)', fontsize=10)
            ax.set_axis_off()
            continue
        xs = sorted(agg)
        ys = [agg[x] for x in xs]
        ax.bar(xs, ys, color='#4C72B0', width=0.9)
        ax.set_yscale('log')
        hwm = max(xs)
        ax.axvline(hwm, color='red', ls='--', lw=1)
        ax.annotate(f'HWM={hwm}', xy=(hwm, max(ys)), xytext=(-4, -4),
                    textcoords='offset points', ha='right', va='top',
                    color='red', fontsize=9)
        ax.set_title(OCC_LABELS[name], fontsize=10)
        ax.set_xlabel('occupancy (concurrent)')
        ax.set_ylabel('samples (log scale)')
        if name == 'blocked_workers':
            total = sum(agg.values())
            busy = (total - agg.get(0, 0)) / total if total else 0.0
            ax.annotate(f'{busy*100:.1f}% of rounds\nhad ≥1 blocked worker',
                        xy=(0.97, 0.92), xycoords='axes fraction', ha='right',
                        va='top', fontsize=9,
                        bbox=dict(boxstyle='round', fc='wheat', alpha=0.6))
    fig.suptitle('Propagation synchronization — occupancy distributions '
                 '(all instances)', fontsize=13)
    fig.tight_layout(rect=[0, 0, 1, 0.96])
    fig.savefig(out_pdf)
    plt.close(fig)


def plot_conflict_views(records, out_pdf):
    """Four complementary views of propagation-lock conflict in one figure.

    (a) rate distribution by lock type (boxplot + per-instance strip)
    (b) wl-insert conflict rate vs. runtime (cross-instance scatter)
    (c) hot lock (wl-insert rate) vs. parallel capacity lost (mean blocked / N)
    (d) aggregate conflict rate by lock type (headline bar)
    """
    if not records:
        return
    types = ['clause', 'wl_insert', 'wl_process']
    labels = ['clause', 'wl-insert', 'wl-process']
    colors = ['#4C72B0', '#DD8452', '#55A868']
    rate_key = {'clause': 'clause_rate', 'wl_insert': 'wl_insert_rate',
                'wl_process': 'wl_process_rate'}
    conf_key = {'clause': 'clause_conflicts', 'wl_insert': 'wl_insert_conflicts',
                'wl_process': 'wl_process_conflicts'}
    acq_key = {'clause': 'clause_acq', 'wl_insert': 'insert_acq', 'wl_process': 'props'}
    rates = {t: [r.get(rate_key[t], 0.0) for r in records] for t in types}

    fig, axes = plt.subplots(2, 2, figsize=(13, 9))
    rng = random.Random(0)  # deterministic strip jitter

    # (a) boxplot + strip of per-instance rate
    axA = axes[0, 0]
    bp = axA.boxplot([rates[t] for t in types], patch_artist=True, showfliers=False)
    for patch, c in zip(bp['boxes'], colors):
        patch.set_facecolor(c)
        patch.set_alpha(0.45)
    for i, t in enumerate(types):
        xs = [i + 1 + rng.uniform(-0.16, 0.16) for _ in rates[t]]
        axA.scatter(xs, rates[t], s=14, color=colors[i], edgecolor='k',
                    linewidth=0.3, alpha=0.7, zorder=3)
    axA.set_xticks(range(1, len(labels) + 1))
    axA.set_xticklabels(labels)
    axA.set_ylabel('per-instance conflict rate (%)')
    axA.set_title('(a) Conflict-rate distribution by lock type')
    axA.grid(axis='y', alpha=0.3)

    # (b) conflict rate vs. runtime (cross-instance): does contention grow with
    # longer/harder solves? (A true intra-run trace needs a finer stat-dump rate.)
    axB = axes[0, 1]
    pts = [(r.get('sim_time_ms', 0.0), r.get('wl_insert_rate', 0.0))
           for r in records if r.get('sim_time_ms', 0.0) > 0]
    if pts:
        axB.scatter([p[0] for p in pts], [p[1] for p in pts], s=26,
                    color='#DD8452', edgecolor='k', linewidth=0.3, alpha=0.75)
        axB.set_xscale('log')
    axB.set_xlabel('runtime (simulated ms, log scale)')
    axB.set_ylabel('wl-insert conflict rate (%)')
    axB.set_title('(b) Conflict rate vs. runtime')
    axB.grid(alpha=0.3, which='both')

    # (c) hot lock vs. parallel capacity lost. y = mean blocked workers / N_WORKERS:
    # if B of N lanes are blocked on average, B/N of the parallel capacity is idle
    # (a throughput-loss proxy, and memory-latency-free since it's an occupancy).
    axC = axes[1, 0]
    xs = [r.get('wl_insert_rate', 0.0) for r in records]
    ys = [100 * r.get('blocked_workers_mean', 0.0) / N_WORKERS for r in records]
    axC.scatter(xs, ys, s=26, color='#DD8452', edgecolor='k', linewidth=0.3, alpha=0.75)
    axC.set_xlabel('wl-insert conflict rate (%)')
    axC.set_ylabel(f'parallel capacity lost (%)  [mean blocked / {N_WORKERS}]')
    axC.set_title('(c) Hot lock vs. parallel capacity lost')
    axC.grid(alpha=0.3)

    # (d) aggregate rate by lock type (totals across instances)
    axD = axes[1, 1]
    agg = []
    for t in types:
        tot_c = sum(r.get(conf_key[t], 0) for r in records)
        tot_a = sum(r.get(acq_key[t], 0) for r in records)
        agg.append(100 * tot_c / tot_a if tot_a else 0.0)
    bars = axD.barh(labels, agg, color=colors)
    axD.invert_yaxis()
    for b, v in zip(bars, agg):
        axD.annotate(f'{v:.3f}%', xy=(v, b.get_y() + b.get_height() / 2),
                     xytext=(4, 0), textcoords='offset points', va='center', fontsize=9)
    axD.set_xlabel('aggregate conflict rate (%)')
    axD.set_title('(d) Aggregate conflict rate by lock type')
    axD.margins(x=0.15)
    axD.grid(axis='x', alpha=0.3)

    fig.suptitle('Propagation-lock conflict — four views '
                 '(rate = stall episodes / acquisitions)', fontsize=13)
    fig.tight_layout(rect=[0, 0, 1, 0.96])
    fig.savefig(out_pdf)
    plt.close(fig)


def plot_hwm_summary(records, out_pdf):
    """Boxplot of per-instance high-water marks per structure (sizing need)."""
    data, labels, maxima = [], [], []
    for name in SYNC_OCC_STATS:
        vals = [r.get(f'{name}_hwm', 0) for r in records if r.get(f'{name}_hwm', 0) > 0]
        data.append(vals if vals else [0])
        labels.append(name.replace('_occ', '').replace('_', ' '))
        maxima.append(max(vals) if vals else 0)
    fig, ax = plt.subplots(figsize=(9, 5))
    bp = ax.boxplot(data, patch_artist=True, showfliers=True)
    for patch in bp['boxes']:
        patch.set_facecolor('#4C72B0')
        patch.set_alpha(0.6)
    ax.set_xticks(range(1, len(labels) + 1))
    ax.set_xticklabels(labels)
    for i, mx in enumerate(maxima):
        ax.annotate(f'max={mx}', xy=(i + 1, mx), xytext=(0, 5),
                    textcoords='offset points', ha='center', fontsize=8, color='red')
    ax.set_ylabel('per-instance high-water mark (concurrent occupancy)')
    ax.set_title('Sync-structure sizing requirement across instances\n'
                 '(box = per-instance HWM distribution; max = worst case to provision)')
    fig.tight_layout()
    fig.savefig(out_pdf)
    plt.close(fig)


def plot_conflict_rate_box(records, out_pdf):
    """Standalone, large-font boxplot + strip of per-instance conflict rate by
    lock type (subplot (a) on its own, for slides/figures)."""
    if not records:
        return
    types = ['clause', 'wl_process', 'wl_insert']
    labels = ['clause', 'WL-\npending', 'WL-\ninsert']
    colors = ['#4C72B0', '#55A868', '#DD8452']
    rate_key = {'clause': 'clause_rate', 'wl_insert': 'wl_insert_rate',
                'wl_process': 'wl_process_rate'}
    rates = {t: [r.get(rate_key[t], 0.0) for r in records] for t in types}

    rng = random.Random(0)
    fig, ax = plt.subplots(figsize=(13, 4.3))
    bp = ax.boxplot([rates[t] for t in types], vert=False, patch_artist=True,
                    showfliers=False, widths=0.6,
                    medianprops=dict(color='black', linewidth=3),
                    whiskerprops=dict(linewidth=2), capprops=dict(linewidth=2),
                    boxprops=dict(linewidth=2))
    for patch, c in zip(bp['boxes'], colors):
        patch.set_facecolor(c)
        patch.set_alpha(0.45)
    for i, t in enumerate(types):
        ys = [i + 1 + rng.uniform(-0.16, 0.16) for _ in rates[t]]
        ax.scatter(rates[t], ys, s=55, color=colors[i], edgecolor='k',
                   linewidth=0.6, alpha=0.7, zorder=3)
    ax.set_yticks(range(1, len(labels) + 1))
    ax.set_yticklabels(labels, fontsize=26, linespacing=0.95)
    ax.invert_yaxis()  # clause on top
    ax.tick_params(axis='x', labelsize=24)
    ax.set_xlabel('conflict rate (%)', fontsize=26)
    ax.grid(axis='x', alpha=0.3)
    fig.tight_layout()
    fig.savefig(out_pdf)
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser(
        description='Extract & plot propagation synchronization-sizing stats '
                    'from a run_sc_l2.sh run folder')
    ap.add_argument('run_folder', help='Run folder (contains seed*/<case>.stats.csv)')
    ap.add_argument('--out-dir', default=None,
                    help='Output directory for CSV/PDFs (default: the run folder)')
    ap.add_argument('--csv', default=None,
                    help='Path for the per-instance CSV summary')
    ap.add_argument('--exclude', default=None,
                    help='Comma-separated substrings; instances whose name '
                         'contains any are skipped (e.g. still-running tests)')
    args = ap.parse_args()

    run_folder = Path(args.run_folder)
    if not run_folder.exists():
        print(f"Error: run folder {run_folder} not found")
        sys.exit(1)
    out_dir = Path(args.out_dir) if args.out_dir else run_folder
    out_dir.mkdir(parents=True, exist_ok=True)

    cases = discover_stats_csvs(run_folder)
    if not cases:
        print(f"No *.stats.csv found under {run_folder}")
        sys.exit(1)

    # Always drop the shared MANUAL_EXCLUSIONS (e.g. did-not-finish tests);
    # --exclude adds further ad-hoc substring filters.
    patterns = [p.strip() for p in (args.exclude or '').split(',') if p.strip()]
    before = len(cases)
    cases = {c: v for c, v in cases.items()
             if c not in MANUAL_EXCLUSIONS and not any(p in c for p in patterns)}
    dropped = before - len(cases)
    if dropped:
        print(f"Excluded {dropped} instance(s) "
              f"(MANUAL_EXCLUSIONS{' + ' + str(patterns) if patterns else ''})")

    records = build_records(cases)
    print(f"Found {len(cases)} instances; {len(records)} with sync stats")
    if not records:
        print("No sync stats present — was --enable-histograms set during the run?")
        sys.exit(1)

    out_csv = Path(args.csv) if args.csv else out_dir / 'race_stats_summary.csv'
    write_csv(records, out_csv)
    print(f"Wrote {out_csv}")

    for fname, fn in (('race_occupancy_distributions.pdf', plot_occupancy_distributions),
                      ('race_conflicts.pdf', plot_conflict_views),
                      ('race_conflict_rate.pdf', plot_conflict_rate_box),
                      ('race_hwm_summary.pdf', plot_hwm_summary)):
        path = out_dir / fname
        fn(records, path)
        print(f"Wrote {path}")

    print("\n=== Aggregate sizing summary ===")
    for name in SYNC_OCC_STATS:
        hwms = sorted(r.get(f'{name}_hwm', 0) for r in records)
        median = hwms[len(hwms) // 2] if hwms else 0
        print(f"  {name:16s} per-instance HWM: max={max(hwms)} median={median}")
    for name in SYNC_CONFLICT_STATS:
        print(f"  {name:20s} total episodes: {sum(r.get(name, 0) for r in records)}")
    asc = sorted(r.get('avg_stalled_per_cycle', 0.0) for r in records)
    if asc and any(asc):
        med = asc[len(asc) // 2]
        print(f"  avg stalled workers/cycle (time-weighted): median={med:.4f} max={max(asc):.4f}"
              f"  (= {100*med/N_WORKERS:.3f}% / {100*max(asc)/N_WORKERS:.3f}% of {N_WORKERS} lanes)")


if __name__ == '__main__':
    main()
