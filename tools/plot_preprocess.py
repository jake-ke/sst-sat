#!/usr/bin/env python3
"""
SAT Preprocessing Comparison Plotter (Minisat + Kissat)

Directly parses and plots the preprocessing comparison for BOTH solvers in a
single invocation. Reuses the parsing / metric / geomean machinery from
plot_comparison.py and carries the preprocess-specific logic (parsing solver
preprocess time, appending a "+ Preproc" column, and a "-Preproc" subtracted
baseline column) directly here -- no separate --preprocess-dir flag needed.

For each solver group the per-solver preprocess time is read from that solver's
baseline log directory (the stereo_{solver}_logs folder passed as the baseline).

Console tables are identical to what plot_comparison.py used to print for the
equivalent per-solver runs. The figure is a single 2-subplot geomean chart:
  (a) Minisat baseline   bars: Minisat, SATBlast, +Preprocess
  (b) Kissat  baseline   bars: Kissat,  SATBlast, +Preprocess
where the "+Preprocess" bar is the opt-final_{solver}_preprocess folder ALONE
(no preprocess time added -- that variant is only shown in the tables).

Example:
  python3 tools/plot_preprocess.py --output-dir results/preprocess \\
      --minisat ../sat-isca26-data/stereo_minisat_logs/ \\
      --kissat  ../sat-isca26-data/stereo_kissat_logs/ \\
      --satblast ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ \\
      --minisat-preprocess ../sat-isca26-data/opt-final_minisat_preprocess/seed3/ \\
      --kissat-preprocess  ../sat-isca26-data/opt-final_kissat_preprocess/seed3/
"""

import sys
import copy
import argparse
from pathlib import Path
from collections import OrderedDict

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.backends.backend_pdf
import matplotlib.patheffects as path_effects
from matplotlib.ticker import MaxNLocator, FormatStrFormatter
from matplotlib.patches import Patch

from unified_parser import parse_log_directory
from plot_comparison import (
    MANUAL_EXCLUSIONS,
    get_folder_colors,
    normalize_test_case,
    compute_metrics_for_folder,
    get_shared_test_set,
    compute_par2_on_shared_set,
    compute_geomean_speedups,
    validate_result_agreement,
)

# Additional hard-coded exclusion specific to the preprocessing comparison.
# Mutates the shared set imported from plot_comparison so get_shared_test_set()
# (which reads that module global at call time) honors it here.
MANUAL_EXCLUSIONS.add("25a654a029421baed232de5e6e19c72e-mp1-qpr-bmp280-driver-14.cnf")


# ---------------------------------------------------------------------------
# Preprocess logic (moved here from plot_comparison.py so it lives directly in
# the preprocessing script).
# ---------------------------------------------------------------------------

def load_preprocess_from_dir(path):
    """Parse a directory of minisat/kissat logs and return a mapping
    {normalized instance name -> preprocess time in ms}.

    Uses unified_parser.parse_log_directory. Two kinds of entries are dropped:
      - logs without a parsed preprocess_time_ms (timeouts/errors that never
        reached the profiling / "Simplification time" line)
      - instances solved entirely by preprocessing (result is SAT/UNSAT but
        zero conflicts were recorded) -- those are not meaningful comparison
        targets since the accelerator never runs on them.
    """
    results = parse_log_directory(Path(path), exclude_summary=True)
    pre_map = {}
    solved_in_prep = []
    for r in results:
        if 'preprocess_time_ms' not in r:
            continue
        tc = r.get('test_case')
        if not tc:
            continue
        try:
            ms = float(r['preprocess_time_ms'])
        except (TypeError, ValueError):
            continue
        # Drop instances solved during preprocessing: SAT/UNSAT with 0 conflicts.
        if r.get('result') in ('SAT', 'UNSAT') and int(r.get('conflicts', 0) or 0) == 0:
            solved_in_prep.append(tc)
            continue
        pre_map[normalize_test_case(tc)] = ms
    if solved_in_prep:
        print(f"Excluded {len(solved_in_prep)} instances solved by preprocessing "
              f"({path}):")
        for tc in solved_in_prep:
            print(f"  {tc}")
    return pre_map


def _recompute_folder_metrics(metrics, timeout_seconds):
    """Recompute par2_score / solved_count / total_count from metrics['results']."""
    timeout_ms = timeout_seconds * 1000.0
    par2_penalty = 2 * timeout_ms
    valid = [r for r in metrics['results'] if r.get('result') not in ('ERROR', 'UNKNOWN')]
    par2_total = 0.0
    solved = 0
    for r in valid:
        res = r.get('result')
        try:
            sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
        except (TypeError, ValueError):
            sim_ms = 0.0
        if res == 'TIMEOUT':
            par2_total += par2_penalty
        elif res in ('SAT', 'UNSAT') and sim_ms <= timeout_ms:
            par2_total += sim_ms
            solved += 1
        else:
            par2_total += par2_penalty
    metrics['par2_score'] = (par2_total / len(valid)) / 1000.0 if valid else None
    metrics['solved_count'] = solved
    metrics['total_count'] = len(valid)
    metrics['excluded_tests'] = [r['test_case'] for r in metrics['results']
                                 if r.get('result') in ('ERROR', 'UNKNOWN')]


def add_baseline_no_preproc(folder_metrics, timeout_seconds, suffix=' -Preproc'):
    """Insert a duplicate of the FIRST folder right after it with preprocess_time_ms
    subtracted from each instance's sim_time_ms. Only instances whose log carried
    a preprocess_time_ms are modified.

    The original baseline is preserved at position 0 (still the geomean reference).
    Returns (new_name, count) or None if folder_metrics is empty.
    """
    if not folder_metrics:
        return None
    base_name = next(iter(folder_metrics))
    base_metrics = folder_metrics[base_name]

    new_results = copy.deepcopy(base_metrics['results'])
    count = 0
    for r in new_results:
        try:
            pp = float(r.get('preprocess_time_ms', 0.0) or 0.0)
        except (TypeError, ValueError):
            pp = 0.0
        if pp <= 0:
            continue
        try:
            sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
        except (TypeError, ValueError):
            sim_ms = 0.0
        r['sim_time_ms'] = max(0.0, sim_ms - pp)
        count += 1

    new_metrics = {
        'results': new_results,
        'par2_score': None,
        'solved_count': 0,
        'total_count': 0,
        'excluded_tests': [],
    }
    _recompute_folder_metrics(new_metrics, timeout_seconds)
    new_name = base_name + suffix

    # Rebuild OrderedDict inserting the variant right after the baseline.
    items = list(folder_metrics.items())
    folder_metrics.clear()
    for k, v in items:
        folder_metrics[k] = v
        if k == base_name:
            folder_metrics[new_name] = new_metrics
    return new_name, count


def apply_preprocess(folder_metrics, pre_map, timeout_seconds, suffix=' + Preproc'):
    """Restrict all folders to instances in pre_map and append a duplicate of the
    last folder with preprocess time added to each instance's sim_time_ms.

    Returns the appended folder name (or None if folder_metrics is empty).
    """
    keep = set(pre_map.keys())
    for metrics in folder_metrics.values():
        metrics['results'] = [r for r in metrics['results']
                              if r.get('test_case') in keep]
        _recompute_folder_metrics(metrics, timeout_seconds)

    if not folder_metrics:
        return None

    last_name = list(folder_metrics.keys())[-1]
    last_metrics = folder_metrics[last_name]

    new_results = copy.deepcopy(last_metrics['results'])
    for r in new_results:
        pre_ms = pre_map.get(r.get('test_case'), 0.0)
        try:
            sim_ms = float(r.get('sim_time_ms', 0.0) or 0.0)
        except (TypeError, ValueError):
            sim_ms = 0.0
        r['sim_time_ms'] = sim_ms + pre_ms

    new_metrics = {
        'results': new_results,
        'par2_score': None,
        'solved_count': 0,
        'total_count': 0,
        'excluded_tests': [],
    }
    _recompute_folder_metrics(new_metrics, timeout_seconds)

    new_name = last_name + suffix
    folder_metrics[new_name] = new_metrics
    return new_name


# ---------------------------------------------------------------------------
# Per-solver processing (mirrors plot_comparison.main for one solver group and
# prints the same tables). Returns plot data.
# ---------------------------------------------------------------------------

def process_solver_group(solver_label, baseline_path, satblast_path,
                         preprocess_path, timeout_seconds,
                         exclude_timeouts=False, errors_as_timeout=False):
    """Run the full preprocess comparison pipeline for one solver and print the
    same tables plot_comparison used to print.

    Folder order: {solver_label} (baseline), SATBlast, +Preprocess.
    The solver's preprocess time is read from its own baseline log directory.

    Returns dict with keys: label, baseline_name, plot_names, geomeans, par2.
    Returns None if fewer than 2 folders yielded valid results.
    """
    print("\n" + "=" * 80)
    print(f"=== {solver_label} ===")
    print("=" * 80)

    folder_specs = [
        (solver_label, baseline_path),
        ('SATBlast', satblast_path),
        ('+Preprocess', preprocess_path),
    ]

    folder_metrics = OrderedDict()
    folder_paths = OrderedDict()
    for name, path in folder_specs:
        print(f"\nProcessing {name} ({path})...")
        metrics = compute_metrics_for_folder(path, timeout_seconds)
        if not metrics['results']:
            print(f"  Warning: No valid results found in {path}")
            continue
        folder_metrics[name] = metrics
        folder_paths[name] = path
        excluded = len(metrics['results']) - metrics['total_count']
        timedout = metrics['total_count'] - metrics['solved_count']
        print(f"  Found {len(metrics['results'])} tests ({excluded} excluded, "
              f"{timedout} timeout), PAR-2: {metrics['par2_score']:.2f}s, "
              f"Solved: {metrics['solved_count']}/{metrics['total_count']}")

    if len(folder_metrics) < 2:
        print(f"\nError: Need at least 2 folders with valid results for {solver_label}")
        return None

    baseline_name = next(iter(folder_metrics.keys()))

    # Preprocess time source = the solver's own baseline log directory.
    pre_map = load_preprocess_from_dir(baseline_path)
    print(f"\nLoaded preprocess times for {len(pre_map)} instances from {baseline_path}")
    preproc_added_name = apply_preprocess(folder_metrics, pre_map, timeout_seconds)
    if preproc_added_name:
        print(f"Added folder '{preproc_added_name}' = last input + preprocess time")
        for fname, metrics in folder_metrics.items():
            if metrics['par2_score'] is None:
                continue
            print(f"  {fname:<30} PAR-2: {metrics['par2_score']:.2f}s  "
                  f"Solved: {metrics['solved_count']}/{metrics['total_count']}")

    # Insert a baseline variant with preprocess time subtracted (tables only).
    alt_baseline_name = None
    result = add_baseline_no_preproc(folder_metrics, timeout_seconds)
    if result:
        alt_baseline_name, count = result
        print(f"\nAdded baseline variant: '{alt_baseline_name}' ({count} instances modified)")

    # Shared test set.
    error_mode = "as timeout" if errors_as_timeout else "excluded"
    print(f"\nDetermining shared test set (exclude_timeouts={exclude_timeouts}, "
          f"errors={error_mode})...")
    shared_tests, exclusion_table = get_shared_test_set(
        folder_metrics, timeout_seconds, exclude_timeouts, errors_as_timeout)

    if exclusion_table:
        print(f"\n=== Excluded Tests ({len(exclusion_table)} tests) ===")
        print(f"{'Excluding Folder':<30} {'Reason':<15} {'Test Case':<100}")
        print("-" * 105)
        for test_case, folder, reason in sorted(exclusion_table):
            print(f"{folder:<30} {reason:<15} {test_case:<100}")

    print(f"Shared test set: {len(shared_tests)} tests")
    print(f"Excluded tests: {len(exclusion_table)}")

    if not shared_tests:
        print(f"Error: No shared tests found across all folders for {solver_label}")
        return None

    mismatches = validate_result_agreement(folder_metrics, shared_tests,
                                           timeout_seconds, folder_paths)

    shared_par2_scores = compute_par2_on_shared_set(
        folder_metrics, shared_tests, timeout_seconds, errors_as_timeout)

    geomean_results = compute_geomean_speedups(
        folder_metrics, shared_tests, timeout_seconds, baseline_name, errors_as_timeout)

    def _print_table(title, ref_name, geomeans, exclude=None):
        print(f"\n{title}")
        print(f"{'Folder':<30} {'PAR-2 (s)':<12} {'Solved/Total':<16} {'Geomean×':<12}")
        print("-" * 74)
        for folder_name in folder_metrics.keys():
            if folder_name == exclude:
                continue
            par2, solved, total = shared_par2_scores[folder_name]
            speed = geomeans.get(folder_name, None)
            if folder_name == ref_name:
                speed = 1.0
            speed_str = f"{speed:.4f}" if speed is not None else 'n/a'
            print(f"{folder_name:<30} {par2:<12.6f} {solved:>8}/{total:<8} {speed_str:<12}")

    _print_table(f"[Geomean vs '{baseline_name}']", baseline_name, geomean_results,
                 exclude=alt_baseline_name)

    if alt_baseline_name and alt_baseline_name in folder_metrics:
        alt_geomeans = compute_geomean_speedups(
            folder_metrics, shared_tests, timeout_seconds, alt_baseline_name, errors_as_timeout)
        _print_table(f"[Geomean vs '{alt_baseline_name}']", alt_baseline_name, alt_geomeans,
                     exclude=baseline_name)

    if mismatches:
        print(f"\n*** RESULT AGREEMENT ERRORS: {len(mismatches)} test(s) have "
              f"SAT/UNSAT mismatches ***")
        for tc, results_map in mismatches:
            parts = [f"{folder}={res}" for folder, res in sorted(results_map.items())]
            print(f"  {tc}: {', '.join(parts)}")
    else:
        print(f"\nResult agreement: OK (all folders agree on SAT/UNSAT for all shared tests)")

    # Plot uses only the three "real" columns (solver, SATBlast, +Preprocess);
    # the "+ Preproc" added-time and "-Preproc" subtracted columns are tables-only.
    plot_names = [baseline_name, 'SATBlast', '+Preprocess']
    plot_names = [n for n in plot_names if n in folder_metrics]

    return {
        'label': solver_label,
        'baseline_name': baseline_name,
        'plot_names': plot_names,
        'geomeans': geomean_results,
        # Name of the "+Preprocess" folder duplicate that has each instance's
        # preprocess time ADDED to its sim_time_ms (created by apply_preprocess).
        # Used to draw the stacked "+ preproc time" portion of the accelerator bar.
        'preproc_added_name': preproc_added_name,
        # Instances actually compared = those NOT solved by preprocessing
        # (solved-in-preprocess instances are dropped by load_preprocess_from_dir).
        'n_tests': len(shared_tests),
    }


# ---------------------------------------------------------------------------
# Combined 2-subplot geomean figure.
# ---------------------------------------------------------------------------

# Color used for the "+Preprocess" bar (overrides the default spectrum color).
PREPROCESS_COLOR = '#9467bd'  # Purple (distinct from Kissat's fixed green)
# Hatch pattern for the stacked "preproc overhead" segment (the headroom between
# the raw and the preproc-included speedup) on the accelerator bar.
PREPROCESS_HATCH = '///'
# Display label for the "+Preprocess" folder (the internal name is kept for
# geomean/color lookup; only the x-tick text changes).
PREPROCESS_DISPLAY = 'Preproc.'


def plot_preprocess_geomean(groups, output_dir):
    """Plot a single figure with two subplots ((a) and (b)), one per solver
    group, each a geomean *normalized-runtime* bar chart vs that solver's
    baseline (software solver runtime = 1.0; lower is better).

    Runtime, not speedup, is plotted because that is the only axis on which the
    preprocessing time stacks honestly (times add; speedups don't). For a
    geometric mean the two views are exact reciprocals -- 1/geomean(runtime) ==
    geomean(speedup) -- so the speedup numbers annotated on the bars are
    identical to the geomean speedups; only the axis changes.

    The "+Preprocess" accelerator bar is drawn as a true additive stack:
      - solid bottom  = 1/R  (R = raw accelerator speedup, no preproc time)
      - hatched top   = 1/W - 1/R  (the preprocessing time, baseline-normalized)
      - full height   = 1/W  (W = speedup once preproc time is charged in)
    so the solid segment reads as the raw speedup R and the whole bar as the
    with-preproc speedup W. Styling matches plot_overall_perf.py.
    """
    # Match plot_overall_perf.py styling.
    font_scale = 2.2
    fig_w = 14 * font_scale
    fig_h = 3.5 * font_scale

    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    pdf_path = output_dir / 'preprocess_geomean.pdf'

    subplot_tags = ['a', 'b', 'c', 'd']

    # 3 bars per subplot. Wider spacing than plot_overall_perf so the
    # horizontal (un-rotated) x-axis labels don't collide.
    bar_width, bar_spacing = 0.7, 1.6

    def _runtime(speed):
        """Normalized runtime = 1/speedup (None passes through)."""
        if speed is None or speed <= 0:
            return None
        return 1.0 / speed

    # Precompute per-group geomean speedups, then derive normalized runtimes and
    # a shared y-axis max so both subplots use the same vertical range.
    group_values = []  # per group: list of (name, raw_speed) for plot_names
    for group in groups:
        baseline_name = group['baseline_name']
        g_values = []
        for name in group['plot_names']:
            speed = group['geomeans'].get(name, None)
            if name == baseline_name:
                speed = 1.0
            g_values.append(speed)
        group_values.append(g_values)

    # Tallest bar = largest normalized runtime, including the with-preproc total
    # (1/W) which is usually the highest since charging preproc time slows it.
    runtime_tops = []
    for group, g_values in zip(groups, group_values):
        preproc_added_name = group.get('preproc_added_name')
        withpp = group['geomeans'].get(preproc_added_name) if preproc_added_name else None
        for name, speed in zip(group['plot_names'], g_values):
            top = _runtime(withpp) if (name == '+Preprocess' and withpp) else _runtime(speed)
            if top is not None:
                runtime_tops.append(top)
    global_max = max(runtime_tops, default=1.0)
    shared_ylim = global_max * 1.30

    with matplotlib.backends.backend_pdf.PdfPages(pdf_path) as pdf:
        fig, axes = plt.subplots(1, len(groups), figsize=(fig_w, fig_h), squeeze=False)
        axes = axes[0]

        for idx, (ax, group, g_values) in enumerate(zip(axes, groups, group_values)):
            names = group['plot_names']

            # With-preproc-time geomean speedup for the accelerator bar (lower
            # than the raw "+Preprocess" speedup, since preproc time is charged).
            preproc_added_name = group.get('preproc_added_name')
            withpp = group['geomeans'].get(preproc_added_name) if preproc_added_name else None

            colors = get_folder_colors(names)
            colors = [PREPROCESS_COLOR if n == '+Preprocess' else c
                      for n, c in zip(names, colors)]
            x_positions = [i * bar_spacing for i in range(len(names))]

            ax.set_ylabel('Norm. runtime', fontsize=int(22 * font_scale))
            ax.tick_params(axis='y', labelsize=int(26 * font_scale))
            ax.grid(axis='y', alpha=0.3)

            for x, name, raw_speed, color in zip(x_positions, names, g_values, colors):
                raw_rt = _runtime(raw_speed)
                # The "+Preprocess" accelerator bar: stack the preproc time on top
                # of the accelerator's own (raw) runtime -- additive in this axis.
                if (name == '+Preprocess' and raw_rt is not None
                        and withpp is not None):
                    withpp_rt = _runtime(withpp)  # = 1/W, the full bar height
                    solid = raw_rt               # = 1/R, accelerator compute only
                    overhead = max(0.0, withpp_rt - solid)  # preproc time, normalized
                    ax.bar(x, solid, width=bar_width, color=color, alpha=0.85,
                           edgecolor='black', linewidth=0.8, zorder=3)
                    ax.bar(x, overhead, width=bar_width, bottom=solid, color=color,
                           alpha=0.85, edgecolor='black', linewidth=0.8,
                           hatch=PREPROCESS_HATCH, zorder=3)
                    # Raw speedup R, just above the solid (accelerator) segment.
                    # The accelerator sliver is thin (preproc dominates runtime),
                    # so the label sits inside the hatched region near its base.
                    # A dark outline (path effect) keeps the bright-green text
                    # legible against the purple hatched bar.
                    ax.text(x, solid, f'{raw_speed:.2f}×', ha='center', va='bottom',
                            fontsize=int(24 * font_scale), color='#39ff14',
                            fontweight='bold',
                            path_effects=[path_effects.withStroke(
                                linewidth=4, foreground='black')])
                    # With-preproc speedup W, above the full bar.
                    ax.text(x, withpp_rt, f'{withpp:.2f}×', ha='center', va='bottom',
                            fontsize=int(22 * font_scale))
                else:
                    val = 0.0 if raw_rt is None else raw_rt
                    ax.bar(x, val, width=bar_width, color=color, alpha=0.85,
                           edgecolor='black', linewidth=0.8, zorder=3)
                    label = 'n/a' if raw_speed is None else f'{raw_speed:.2f}×'
                    ax.text(x, val, label, ha='center', va='bottom',
                            fontsize=int(22 * font_scale))

            # Two separate legends (kept apart, not merged, no overlap): the
            # stacked-segment key on the right, and the instance count just to
            # its left.
            if withpp is not None:
                seg_legend = ax.legend(
                    handles=[
                        Patch(facecolor=PREPROCESS_COLOR, edgecolor='black', alpha=0.85,
                              label='Accelerator'),
                        Patch(facecolor=PREPROCESS_COLOR, edgecolor='black', alpha=0.85,
                              hatch=PREPROCESS_HATCH, label='Host Preproc.'),
                    ],
                    loc='upper right', bbox_to_anchor=(1.0, 1.0),
                    fontsize=int(16 * font_scale), frameon=True)
                ax.add_artist(seg_legend)
            ax.legend(handles=[Patch(facecolor='none', edgecolor='none',
                                     label=f"{group['n_tests']} instances")],
                      loc='upper right', bbox_to_anchor=(0.60, 1.0),
                      fontsize=int(16 * font_scale),
                      frameon=True, handlelength=0, handletextpad=0)

            display_names = [PREPROCESS_DISPLAY if n == '+Preprocess' else n for n in names]
            ax.set_xticks(x_positions)
            ax.set_xticklabels(display_names, fontsize=int(26 * font_scale), ha='center')
            ax.set_ylim(0, shared_ylim)
            ax.yaxis.set_major_locator(MaxNLocator(nbins=5))
            ax.yaxis.set_major_formatter(FormatStrFormatter('%.1f'))

            ax.set_xlabel(f"({subplot_tags[idx]}) {group['label']}",
                          fontsize=int(28 * font_scale), fontweight='bold', labelpad=12)

        plt.tight_layout()
        pdf.savefig(fig, bbox_inches='tight')
        plt.close(fig)

    print(f"\nPreprocess geomean chart saved to: {pdf_path}")


def main():
    parser = argparse.ArgumentParser(
        description='Plot preprocessing comparison for Minisat and Kissat in one figure',
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument('--minisat', required=True, metavar='DIR',
                        help='Minisat baseline log directory (also the preprocess-time source)')
    parser.add_argument('--kissat', required=True, metavar='DIR',
                        help='Kissat baseline log directory (also the preprocess-time source)')
    parser.add_argument('--satblast', required=True, metavar='DIR',
                        help='SATBlast accelerator results directory (shared by both solvers)')
    parser.add_argument('--minisat-preprocess', required=True, metavar='DIR',
                        help='SATBlast-on-preprocessed-CNF results for Minisat (opt-final_minisat_preprocess)')
    parser.add_argument('--kissat-preprocess', required=True, metavar='DIR',
                        help='SATBlast-on-preprocessed-CNF results for Kissat (opt-final_kissat_preprocess)')
    parser.add_argument('--timeout', type=float, default=36,
                        help='Timeout in seconds for PAR-2 calculation (default: 36)')
    parser.add_argument('--output-dir', default='results',
                        help='Output directory for plots (default: results/)')
    parser.add_argument('--exclude-timeouts-geomean', action='store_true',
                        help='Exclude TIMEOUT tests from geomean speedup calculation')
    parser.add_argument('--errors-as-timeout', action='store_true',
                        help='Treat ERROR/UNKNOWN as timeout (1×timeout penalty) instead of excluding them')

    args = parser.parse_args()

    print(f"Preprocess comparison with timeout={args.timeout}s")

    solver_inputs = [
        ('Minisat', args.minisat, args.minisat_preprocess),
        ('Kissat', args.kissat, args.kissat_preprocess),
    ]

    groups = []
    for solver_label, baseline_path, preprocess_path in solver_inputs:
        group = process_solver_group(
            solver_label, baseline_path, args.satblast, preprocess_path,
            args.timeout, args.exclude_timeouts_geomean, args.errors_as_timeout)
        if group:
            groups.append(group)

    if not groups:
        print("\nError: No solver groups produced valid results")
        sys.exit(1)

    plot_preprocess_geomean(groups, args.output_dir)


if __name__ == "__main__":
    main()
