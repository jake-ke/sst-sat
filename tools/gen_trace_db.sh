#!/bin/bash
# Generate a trace database: a small fixed set of CNFs crossed with several
# seeds, each run producing one .trace.bin + .stats.csv + .log.
#
# Layout:
#   runs/trace_db/<cnf_stem>/seed<N>.trace.bin
#   runs/trace_db/<cnf_stem>/seed<N>.stats.csv
#   runs/trace_db/<cnf_stem>/seed<N>.sim.log
#
# Intended for rapid correctness / practicality verification of the
# binary trace writer (see TRACE_FORMAT.md).

set -u

# ---- Config ----------------------------------------------------------------

BENCH_DIR="${BENCH_DIR:-/home/jakeke/sat_benchmarks/satcomp_sim}"
DB_DIR="${DB_DIR:-./runs/trace_db}"
TIMEOUT_CYCLES="${TIMEOUT_CYCLES:-36000000000}"   # 36B cycles ~= 36 ms simulated @ 1GHz
NUM_SEEDS="${NUM_SEEDS:-4}"
JOBS="${JOBS:-4}"

# Hardware config defaults match the opt-final_l1_4_1_l2_8_32 profile used by
# run_sc_l2.sh. Any of these can be overridden by exporting the env var.
# Set any to the empty string to fall back to test_two_level.py defaults.
RAM2_CFG="${RAM2_CFG-tests/ramulator2-ddr4.cfg}"
L1_SIZE="${L1_SIZE-128KiB}"
L1_LATENCY="${L1_LATENCY-1}"
L1_BW="${L1_BW-4}"
L2_LATENCY="${L2_LATENCY-32}"
L2_BW="${L2_BW-8}"
PREFETCH="${PREFETCH-1}"

# Default CNF list: every *.cnf in BENCH_DIR if CNFS is unset.
# Override with e.g.  CNFS="a.cnf b.cnf" tools/gen_trace_db.sh
if [[ -z "${CNFS:-}" ]]; then
    if [[ ! -d "$BENCH_DIR" ]]; then
        echo "error: BENCH_DIR='$BENCH_DIR' does not exist; set BENCH_DIR or CNFS" >&2
        exit 1
    fi
    CNFS=$(cd "$BENCH_DIR" && ls *.cnf 2>/dev/null | tr '\n' ' ')
fi

mkdir -p "$DB_DIR"

# ---- Color output ----------------------------------------------------------

if [[ -t 1 ]]; then
    RED=$'\033[0;31m'; GREEN=$'\033[0;32m'; YELLOW=$'\033[0;33m'; NC=$'\033[0m'
else
    RED=''; GREEN=''; YELLOW=''; NC=''
fi

# ---- Build task list -------------------------------------------------------

tasks=()
for cnf_name in $CNFS; do
    if [[ "$cnf_name" = /* ]]; then
        cnf_path="$cnf_name"
    else
        cnf_path="$BENCH_DIR/$cnf_name"
    fi
    if [[ ! -f "$cnf_path" ]]; then
        echo "${YELLOW}skip${NC}: $cnf_path not found"
        continue
    fi
    cnf_stem=$(basename "$cnf_path" .cnf)
    mkdir -p "$DB_DIR/$cnf_stem"
    for ((seed=0; seed<NUM_SEEDS; seed++)); do
        tasks+=("$cnf_path|$cnf_stem|$seed")
    done
done

total=${#tasks[@]}
if [[ $total -eq 0 ]]; then
    echo "error: no tasks to run (check BENCH_DIR and CNFS)" >&2
    exit 1
fi

echo "=============================================================="
echo " Trace database generator"
echo " bench_dir      : $BENCH_DIR"
echo " db_dir         : $DB_DIR"
echo " num_seeds      : $NUM_SEEDS"
echo " timeout_cycles : $TIMEOUT_CYCLES"
echo " parallel jobs  : $JOBS"
echo " ram2_cfg       : ${RAM2_CFG:-<simpleMem default>}"
echo " l1_size        : ${L1_SIZE:-<default>}"
echo " l1_latency     : ${L1_LATENCY:-<default>}"
echo " l1_bw          : ${L1_BW:-<default>}"
echo " l2_latency     : ${L2_LATENCY:-<default>}"
echo " l2_bw          : ${L2_BW:-<default>}"
echo " prefetch       : ${PREFETCH:+enabled}"
echo " tasks          : $total"
echo "=============================================================="

# ---- Runner ---------------------------------------------------------------

run_one() {
    local cnf_path="$1"
    local cnf_stem="$2"
    local seed="$3"

    local out_dir="$DB_DIR/$cnf_stem"
    local trace_file="$out_dir/seed${seed}.trace.bin"
    local stats_file="$out_dir/seed${seed}.stats.csv"
    local log_file="$out_dir/seed${seed}.sim.log"

    if [[ -s "$trace_file" && -s "$stats_file" ]]; then
        echo "${YELLOW}skip${NC} $cnf_stem seed=$seed (already present)"
        return 0
    fi

    local t0=$(date +%s)
    local extra=()
    [[ -n "$RAM2_CFG"    ]] && extra+=(--ram2-cfg "$RAM2_CFG")
    [[ -n "$L1_SIZE"     ]] && extra+=(--l1-size "$L1_SIZE")
    [[ -n "$L1_LATENCY"  ]] && extra+=(--l1-latency "$L1_LATENCY")
    [[ -n "$L1_BW"       ]] && extra+=(--l1-bw "$L1_BW")
    [[ -n "$L2_LATENCY"  ]] && extra+=(--l2-latency "$L2_LATENCY")
    [[ -n "$L2_BW"       ]] && extra+=(--l2-bw "$L2_BW")
    [[ -n "$PREFETCH"    ]] && extra+=(--prefetch)

    sst tests/test_two_level.py -- \
        --cnf "$cnf_path" \
        --rand "$seed" \
        --trace-file "$trace_file" \
        --stats-file "$stats_file" \
        --timeout-cycles "$TIMEOUT_CYCLES" \
        "${extra[@]}" \
        >"$log_file" 2>&1
    local rc=$?
    local t1=$(date +%s)
    local dt=$((t1 - t0))

    local status
    if [[ $rc -ne 0 ]]; then
        status="${RED}FAIL${NC}(rc=$rc)"
    elif grep -q "SATISFIABLE: All variables assigned" "$log_file"; then
        status="${GREEN}SAT${NC}"
    elif grep -q "UNSATISFIABLE" "$log_file"; then
        status="${GREEN}UNSAT${NC}"
    elif grep -q "Timeout Reached" "$log_file"; then
        status="${YELLOW}TIMEOUT${NC}"
    else
        status="${RED}UNKNOWN${NC}"
    fi

    local size="-"
    if [[ -f "$trace_file" ]]; then
        size=$(stat -c%s "$trace_file")
    fi
    printf "%s %-60s seed=%d  %6ss  trace=%10s B\n" \
        "$status" "$cnf_stem" "$seed" "$dt" "$size"
}

export -f run_one
export DB_DIR RED GREEN YELLOW NC TIMEOUT_CYCLES
export RAM2_CFG L1_SIZE L1_LATENCY L1_BW L2_LATENCY L2_BW PREFETCH

# Serial if JOBS==1, else xargs-based parallel.
if [[ $JOBS -le 1 ]]; then
    for task in "${tasks[@]}"; do
        IFS='|' read -r cnf_path cnf_stem seed <<<"$task"
        run_one "$cnf_path" "$cnf_stem" "$seed"
    done
else
    printf '%s\n' "${tasks[@]}" | \
        xargs -I{} -P "$JOBS" bash -c '
            IFS="|" read -r cnf_path cnf_stem seed <<<"$1"
            run_one "$cnf_path" "$cnf_stem" "$seed"
        ' _ {}
fi

echo "=============================================================="
echo " Database contents:"
find "$DB_DIR" -name "*.trace.bin" | sort | while read f; do
    printf "  %-70s %10s B\n" "$f" "$(stat -c%s "$f")"
done
echo "=============================================================="