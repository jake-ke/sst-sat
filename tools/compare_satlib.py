#!/usr/bin/env python3
"""
Compare MiniSat (our simulator) SATLIB results against VeriSAT@150MHz results.

Parses our results from a run directory using unified_parser, compares against
hardcoded VeriSAT averages from published data, and prints a summary table
grouped by dataset.

Usage:
    python compare_satlib.py <run_dir> [--timeout SECONDS]

Example:
    python compare_satlib.py runs/opt-final_satlib --timeout 36
"""

import argparse
import re
import sys
from pathlib import Path
from collections import defaultdict

from unified_parser import parse_log_directory


# SATComp tests that exceed VeriSAT's hardware limits (16,384 vars or 1,048,576 clauses)
# These are from ~/sat_benchmarks/satcomp_sim, excluding 3 manual exclusions
LARGE_SATCOMP_TESTS = [
    {"name": "06e928088bd822602edb83e41ce8dadb-satcoin-genesis-SAT-10.cnf",                             "vars": 134780,  "clauses": 648803},
    {"name": "1427381a809c64c721838894ece6756d-shuffling-2-s25242449-of-bench-sat04-727.used-as.sat04-753.cnf", "vars": 138711, "clauses": 4688614},
    {"name": "25a654a029421baed232de5e6e19c72e-mp1-qpr-bmp280-driver-14.cnf",                          "vars": 23325,   "clauses": 95362},
    {"name": "43e492bccfd57029b758897b17d7f04f-pb_300_09_lb_07.cnf",                                   "vars": 145943,  "clauses": 654778},
    {"name": "7aa3b29dde431cdacf17b2fb9a10afe4-Mario-t-hard-2_c18.cnf",                                "vars": 35945,   "clauses": 1218500},
    {"name": "7b5895f110a6aa5a5ac5d5a6eb3fd322-g2-ak128modasbg1sbisc.cnf",                             "vars": 343799,  "clauses": 1128030},
    {"name": "91c69a1dedfa5f2b3779e38c48356593-Problem14_label20_true-unreach-call.c.cnf",              "vars": 1703806, "clauses": 7317082},
    {"name": "aacfb8797097f698d14337d3a04f3065-barman-pfile06-022.sas.ex.7.cnf",                       "vars": 127192,  "clauses": 284930},
    {"name": "ca14adcb9296a7b31d7815c2ed16d0f1-ITC2021_Early_3.xml.cnf",                               "vars": 29478,   "clauses": 125980},
    {"name": "fce130da1efd25fa9b12e59620a4146b-g2-ak128diagobg1btaig.cnf",                             "vars": 536817,  "clauses": 1608156},
    {"name": "ff6b6ad55c0dffe034bc8daa9129ff1d-satcoin-genesis-SAT-10-sc2018.cnf",                     "vars": 134780,  "clauses": 648803},
]


# VeriSAT@150MHz published results (average across all instances)
# Columns: dataset, #instances, avg_cycles, avg_time_ms, timeouts
VERISAT_DATA = {
    "UF20":   {"instances": 1000, "avg_cycles": 2803.15,       "avg_time_ms": 0.02,    "timeouts": 0},
    "UF50":   {"instances": 1000, "avg_cycles": 39240.15,      "avg_time_ms": 0.26,    "timeouts": 0},
    "UF75":   {"instances": 100,  "avg_cycles": 298172.04,     "avg_time_ms": 1.99,    "timeouts": 0},
    "UF100":  {"instances": 1000, "avg_cycles": 1712141.66,    "avg_time_ms": 11.41,   "timeouts": 0},
    "UF125":  {"instances": 100,  "avg_cycles": 16593148.63,   "avg_time_ms": 110.62,  "timeouts": 0},
    "UF150":  {"instances": 100,  "avg_cycles": 107043051.36,  "avg_time_ms": 713.62,  "timeouts": 18},
    "UUF50":  {"instances": 1000, "avg_cycles": 91667.29,      "avg_time_ms": 0.61,    "timeouts": 0},
    "UUF75":  {"instances": 100,  "avg_cycles": 633991.85,     "avg_time_ms": 4.23,    "timeouts": 0},
    "UUF100": {"instances": 999,  "avg_cycles": 5110190.56,    "avg_time_ms": 34.07,   "timeouts": 0},
    "UUF125": {"instances": 100,  "avg_cycles": 39052766.50,   "avg_time_ms": 260.35,  "timeouts": 0},
    "UUF150": {"instances": 100,  "avg_cycles": 143538806.50,  "avg_time_ms": 956.93,  "timeouts": 0},
    "QG":     {"instances": 22,   "avg_cycles": 27193919.44,   "avg_time_ms": 181.29,  "timeouts": 1},
    "BMC":    {"instances": 13,   "avg_cycles": 2709938.33,    "avg_time_ms": 18.07,   "timeouts": 0},
    "HOLE":   {"instances": 5,    "avg_cycles": 10474512.25,   "avg_time_ms": 69.83,   "timeouts": 0},
    "II":     {"instances": 41,   "avg_cycles": 1594420.02,    "avg_time_ms": 10.63,   "timeouts": 0},
    "LOGISTICS": {"instances": 4, "avg_cycles": 2408013.05,    "avg_time_ms": 16.05,   "timeouts": 0},
}


def fmt_compact(val):
    """Format number compactly for LaTeX (e.g., 63624 -> '64k')."""
    if val >= 2000:
        return f"{round(val / 1000)}k"
    return str(val)


def fmt_range_compact(vals):
    """Format a var/clause range compactly for LaTeX."""
    lo, hi = min(vals), max(vals)
    if lo == hi:
        return str(lo)
    return f"{fmt_compact(lo)}--{fmt_compact(hi)}"


def classify_dataset(test_name):
    """Classify a test case name into its SATLIB dataset group."""
    name = test_name.lower()
    # Remove .cnf extension if present
    name = re.sub(r'\.cnf$', '', name)

    # Order matters: check longer prefixes first
    if name.startswith("uuf"):
        m = re.match(r'uuf(\d+)', name)
        if m:
            return f"UUF{m.group(1)}"
    elif name.startswith("uf"):
        m = re.match(r'uf(\d+)', name)
        if m:
            return f"UF{m.group(1)}"
    elif name.startswith("qg"):
        return "QG"
    elif name.startswith("bmc"):
        return "BMC"
    elif name.startswith("hole"):
        return "HOLE"
    elif name.startswith("ii"):
        return "II"
    elif name.startswith("logistics"):
        return "LOGISTICS"
    return None


def main():
    parser = argparse.ArgumentParser(description="Compare SATLIB results against VeriSAT")
    parser.add_argument("run_dir", help="Path to run directory (e.g., runs/opt-final_satlib)")
    parser.add_argument("--timeout", type=float, default=36.0,
                        help="Timeout in seconds (default: 36)")
    parser.add_argument("--latex", action="store_true",
                        help="Print LaTeX tables at the end")
    args = parser.parse_args()

    run_dir = Path(args.run_dir)
    timeout_ms = args.timeout * 1000.0

    # Find the seed directory
    seed_dirs = sorted(run_dir.glob("seed*"))
    if not seed_dirs:
        print(f"Error: No seed directories found in {run_dir}")
        sys.exit(1)

    # Parse all log files
    print(f"Parsing results from {run_dir}...")
    all_results = []
    for seed_dir in seed_dirs:
        results = parse_log_directory(str(seed_dir))
        all_results.extend(results)
    print(f"Parsed {len(all_results)} log files")

    # Group by dataset
    datasets = defaultdict(list)
    unclassified = []
    for r in all_results:
        test_name = r.get('test_case', '')
        ds = classify_dataset(test_name)
        if ds:
            datasets[ds].append(r)
        else:
            unclassified.append(test_name)

    if unclassified:
        print(f"Warning: {len(unclassified)} unclassified tests: {unclassified[:5]}...")

    # Define dataset ordering to match VeriSAT table
    dataset_order = [
        "UF20", "UF50", "UF75", "UF100", "UF125", "UF150",
        "UUF50", "UUF75", "UUF100", "UUF125", "UUF150",
        "QG", "BMC", "HOLE", "II", "LOGISTICS",
    ]

    # VeriSAT scaling factor (÷8 — scale down runtime)
    VERISAT_SCALE = 1.0 / 8.0

    latex_rows = []  # collect for LaTeX output
    latex_inst = 0
    latex_verisat_to = 0
    latex_ours_to = 0
    latex_speedups = []

    # Print header
    header = (
        f"{'Dataset':<12} "
        f"{'#Var. Range':>16} "
        f"{'#Cl. Range':>16} "
        f"{'#Inst':>6} "
        f"{'VeriSAT@150MHz(ms)':>17} "
        f"{'VeriSAT TO':>10} "
        f"{'VeriSAT@950MHz(ms)':>15} "
        f"{'SATBlast Time(ms)':>17} "
        f"{'SATBlast TO':>10} "
        f"{'Speedup@150MHz':>13} "
        f"{'Speedup@950MHz':>11}"
    )
    separator = "-" * len(header)
    print()
    print(header)
    print(separator)

    all_speedups_150 = []
    all_speedups_scaled = []
    total_verisat_to = 0
    total_ours_to = 0
    total_inst = 0

    for ds in dataset_order:
        our_tests = datasets.get(ds, [])
        verisat = VERISAT_DATA.get(ds)

        if not verisat:
            continue

        # Compute our stats
        n_inst = len(our_tests)
        timeouts = sum(1 for r in our_tests if r.get('result') in ('TIMEOUT', 'UNKNOWN'))
        times = [r['sim_time_ms'] for r in our_tests if r.get('sim_time_ms', 0) > 0]

        # Compute var and clause ranges
        vars_list = [r['variables'] for r in our_tests if r.get('variables', 0) > 0]
        cls_list = [r['clauses'] for r in our_tests if r.get('clauses', 0) > 0]
        if vars_list:
            var_range = f"{min(vars_list)} - {max(vars_list)}"
        else:
            var_range = "N/A"
        if cls_list:
            cl_range = f"{min(cls_list)} - {max(cls_list)}"
        else:
            cl_range = "N/A"

        if times:
            avg_time = sum(times) / len(times)
        else:
            avg_time = 0.0

        verisat_150 = verisat['avg_time_ms']
        verisat_scaled = verisat_150 * VERISAT_SCALE

        # Speedup: VeriSAT time / our time
        if avg_time > 0:
            speedup_150 = verisat_150 / avg_time
            speedup_scaled = verisat_scaled / avg_time
        else:
            speedup_150 = float('inf')
            speedup_scaled = float('inf')

        speedup_150_str = f"{speedup_150:.1f}"
        speedup_scaled_str = f"{speedup_scaled:.1f}"

        # Collect per-dataset speedups for overall average
        if avg_time > 0 and speedup_150 != float('inf'):
            all_speedups_150.append(speedup_150)
            all_speedups_scaled.append(speedup_scaled)
        total_verisat_to += verisat['timeouts']
        total_ours_to += timeouts
        total_inst += n_inst

        # Collect LaTeX row (skip HOLE — flawed benchmarks)
        if ds != "HOLE":
            var_compact = fmt_range_compact(vars_list) if vars_list else "N/A"
            cl_compact = fmt_range_compact(cls_list) if cls_list else "N/A"
            latex_rows.append(
                f"{ds} & {var_compact} & {cl_compact} & {n_inst} & "
                f"{verisat_scaled:.1f} & {avg_time:.1f} & "
                f"{speedup_scaled_str} & {verisat['timeouts']}/{timeouts} \\\\"
            )
            latex_inst += n_inst
            latex_verisat_to += verisat['timeouts']
            latex_ours_to += timeouts
            if avg_time > 0 and speedup_scaled != float('inf'):
                latex_speedups.append(speedup_scaled)

        print(
            f"{ds:<12} "
            f"{var_range:>16} "
            f"{cl_range:>16} "
            f"{n_inst:>6} "
            f"{verisat_150:>17.2f} "
            f"{verisat['timeouts']:>10} "
            f"{verisat_scaled:>15.2f} "
            f"{avg_time:>17.2f} "
            f"{timeouts:>10} "
            f"{speedup_150_str:>13} "
            f"{speedup_scaled_str:>11}"
        )

    print(separator)

    # Large SATComp tests that exceed VeriSAT's hardware limits
    n_large = len(LARGE_SATCOMP_TESTS)
    large_vars = [t["vars"] for t in LARGE_SATCOMP_TESTS]
    large_cls = [t["clauses"] for t in LARGE_SATCOMP_TESTS]
    large_var_range = f"{min(large_vars)} - {max(large_vars)}"
    large_cl_range = f"{min(large_cls)} - {max(large_cls)}"

    # VeriSAT cannot run these — all are TOs
    total_verisat_to += n_large
    total_inst += n_large

    print(
        f"{'Lg. SATComp':<12} "
        f"{large_var_range:>16} "
        f"{large_cl_range:>16} "
        f"{n_large:>6} "
        f"{'N/A':>17} "
        f"{n_large:>10} "
        f"{'N/A':>15} "
        f"{'N/A':>17} "
        f"{'N/A':>10} "
        f"{'N/A':>13} "
        f"{'N/A':>11}"
    )

    print(separator)

    # Overall speedup = simple average of per-dataset speedups
    avg_150 = sum(all_speedups_150) / len(all_speedups_150) if all_speedups_150 else 0.0
    avg_scaled = sum(all_speedups_scaled) / len(all_speedups_scaled) if all_speedups_scaled else 0.0

    print(
        f"{'AVERAGE':<12} "
        f"{'':>16} "
        f"{'':>16} "
        f"{total_inst:>6} "
        f"{'':>17} "
        f"{total_verisat_to:>10} "
        f"{'':>15} "
        f"{'':>17} "
        f"{total_ours_to:>10} "
        f"{avg_150:>13.2f} "
        f"{avg_scaled:>11.2f}"
    )

    print(separator)

    # =========================================================================
    # SATHard comparison (per-instance, unknown frequency so no scaling)
    # =========================================================================
    # From screenshot: VS = VeriSAT(ms), SA = SATAccel(ms), SH = SATHard(ms)
    # We compare only to SATHard (SH column)
    SATHARD_DATA = [
        {"name": "hole7",       "our_name": "hole7.cnf",                  "vars": 56,  "clauses": 204,  "sh_ms": 330},
        {"name": "hole8",       "our_name": "hole8.cnf",                  "vars": 72,  "clauses": 297,  "sh_ms": 2270},
        {"name": "hole9",       "our_name": "hole9.cnf",                  "vars": 90,  "clauses": 415,  "sh_ms": 15290},
        {"name": "uf100-010",   "our_name": "uf100-010.cnf",              "vars": 100, "clauses": 430,  "sh_ms": 580},
        {"name": "uf125-01",    "our_name": "uf125-01.cnf",               "vars": 125, "clauses": 538,  "sh_ms": 1160},
        {"name": "uf150-08",    "our_name": "uf150-08.cnf",               "vars": 150, "clauses": 645,  "sh_ms": 3920},
        {"name": "uuf100-02",   "our_name": "uuf100-02.cnf",              "vars": 100, "clauses": 430,  "sh_ms": 4940},
        {"name": "uuf125-05",   "our_name": "uuf125-05.cnf",              "vars": 125, "clauses": 538,  "sh_ms": 4900},
        {"name": "CBS-k3-n100-m403-k3-10", "our_name": "CBS_k3_n100_m403_b10_1.cnf", "vars": 100, "clauses": 403, "sh_ms": 2340},
        {"name": "aim-200-3_4-yes1-4",     "our_name": "aim-200-3_4-yes1-4.cnf",      "vars": 200, "clauses": 320, "sh_ms": 1200},
        {"name": "aim-200-1_6-no-4",       "our_name": "aim-200-1_6-no-4.cnf",        "vars": 200, "clauses": 320, "sh_ms": 10},
        {"name": "ii16e2",      "our_name": "ii16e2.cnf",                 "vars": 222, "clauses": 1186, "sh_ms": 5760},
        {"name": "ii32e1",      "our_name": "ii32e1.cnf",                 "vars": 222, "clauses": 1186, "sh_ms": 20},
    ]

    # SATHard frequency scaling factor (10x — scale down runtime)
    SATHARD_SCALE = 1.0 / 10.0

    # Build lookup from our parsed results
    our_lookup = {}
    for r in all_results:
        tc = r.get('test_case', '')
        our_lookup[tc + '.cnf'] = r
        our_lookup[tc] = r

    sh_latex_rows = []

    print("=" * 100)
    print("SATHard Comparison (per-instance)")
    print("=" * 100)

    sh_header = (
        f"{'Problem':<30} "
        f"{'Var':>6} "
        f"{'Cls':>8} "
        f"{'SATHard(ms)':>12} "
        f"{'SATHard/10(ms)':>16} "
        f"{'SATBlast(ms)':>12} "
        f"{'Speedup':>10}"
    )
    sh_sep = "-" * len(sh_header)
    print(sh_header)
    print(sh_sep)

    sh_speedups = []

    for t in SATHARD_DATA:
        our = our_lookup.get(t["our_name"], {})
        our_time = our.get('sim_time_ms', 0.0)

        sh_scaled = t['sh_ms'] * SATHARD_SCALE
        sh_str = f"{sh_scaled:.1f}"
        our_str = f"{our_time:.1f}" if our_time > 0 else "N/A"

        if our_time > 0:
            sp = sh_scaled / our_time
            sp_str = f"{sp:.1f}"
            sh_speedups.append(sp)
        else:
            sp_str = "N/A"

        # Collect SATHard LaTeX row (escape underscores for LaTeX)
        latex_name = t['name'].replace('_', r'\_')
        sh_latex_rows.append(
            f"{latex_name} & {t['vars']} & {t['clauses']} & "
            f"{sh_str} & {our_str} & {sp_str} \\\\"
        )

        print(
            f"{t['name']:<30} "
            f"{t['vars']:>6} "
            f"{t['clauses']:>8} "
            f"{t['sh_ms']:>12.2f} "
            f"{sh_str:>16} "
            f"{our_str:>12} "
            f"{sp_str:>10}"
        )

    print(sh_sep)

    avg_sp = sum(sh_speedups) / len(sh_speedups) if sh_speedups else 0.0

    print(
        f"{'AVERAGE':<30} "
        f"{'':>6} "
        f"{'':>8} "
        f"{'':>12} "
        f"{'':>16} "
        f"{'':>12} "
        f"{avg_sp:>10.2f}"
    )
    print(sh_sep)

    # =========================================================================
    # LaTeX output (--latex)
    # =========================================================================
    if args.latex:
        print()
        print("% ============ LaTeX Table: VeriSAT Comparison ============")
        print(r"\begin{table}[h]")
        print(r"\centering")
        print(r"\caption{Comparison with VeriSAT on SATLIB benchmarks.}")
        print(r"\label{tab:verisat}")
        print(r"\small")
        print(r"\setlength{\tabcolsep}{1.5pt}")
        print(r"\renewcommand{\arraystretch}{0.9}")
        print(r"\begin{tabular}{@{}lrrrrrrr@{}}")
        print(r"\toprule")
        print(r"Dataset & \#Var & \#Cl & \#Inst & \makecell{VeriSAT\\$\div$8 (ms)} & \makecell{SATBlast\\(ms)} & \makecell{Speed-\\up} & TO \\")
        print(r"\midrule")
        for row in latex_rows:
            print(row)
        print(r"\midrule")
        latex_avg = sum(latex_speedups) / len(latex_speedups) if latex_speedups else 0.0
        print(
            f"\\textbf{{Average}} & & & {latex_inst} & "
            f"& & {latex_avg:.1f} & "
            f"{latex_verisat_to}/{latex_ours_to} \\\\"
        )
        print(r"\bottomrule")
        print(r"\end{tabular}")
        print(r"\end{table}")
        print()

        print("% ============ LaTeX Table: SATHard Comparison ============")
        print(r"\begin{table}[h]")
        print(r"\centering")
        print(r"\caption{Comparison with SATHard.}")
        print(r"\label{tab:sathard}")
        print(r"\small")
        print(r"\setlength{\tabcolsep}{3pt}")
        print(r"\begin{tabular}{@{}lrrrrr@{}}")
        print(r"\toprule")
        print(r" & & & SATHard & SATBlast & \\")
        print(r"Problem & \#Var & \#Cl & $\div$10 (ms) & (ms) & Speedup \\")
        print(r"\midrule")
        for row in sh_latex_rows:
            print(row)
        print(r"\midrule")
        print(f"\\textbf{{Average}} & & & & & {avg_sp:.1f} \\\\")
        print(r"\bottomrule")
        print(r"\end{tabular}")
        print(r"\end{table}")
        print()


if __name__ == "__main__":
    main()
