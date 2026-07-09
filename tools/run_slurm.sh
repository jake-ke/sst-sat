#!/bin/bash
# One-script slurm dispatcher for SAT-simulator benchmark sweeps.
#
# Unlike the old sbatch approach (allocate a whole node, run run_sc_l2.sh -j16
# inside), every (cnf, seed) pair becomes its OWN 1-CPU array task, so:
#   - finished instances release their allocation immediately (no node held
#     hostage while a few long instances finish);
#   - results stream directly into runs/<folder>/seedN/ in THIS repo (no
#     end-of-job copy to lose when slurm kills the job at its time limit);
#   - re-submitting the same command skips instances that already have a .log
#     (same resume convention as run_sc_l2.sh).
#
# Conventions kept from run_sc_l2.sh: output layout runs/<folder>/seed<S>/
# with <name>.log + <name>.stats.csv, parse_stats.py appended to the log,
# verifier.py checked for SAT verdicts, PASSED/FAILED/TIMEOUT classification.
# The element binary is whatever `make -C src install` registered from this
# repo (e.g. ~/sst-sat/build) -- build it BEFORE submitting; no scratch clones.
#
# Usage (from the repo root on the slurm login node):
#   tools/run_slurm.sh -b ~/sat_benchmarks/satcomp_sim --folder singly-fl --seed 3 \
#       [--max-parallel 64] [--task-mem 6G] [--task-time D-HH:MM:SS] [--partition batch] \
#       [--sbatch-opts "--qos=low --requeue"] \
#       -- --ram2-cfg tests/ramulator2-ddr4.cfg --l1-size 128KiB --l1-latency 1 \
#          --l1-bw 4 --l2-latency 32 --l2-bw 8 --prefetch --timeout-cycles 36000000000
#
#   Everything after `--` is passed verbatim to tests/test_two_level.py
#   (do NOT pass --cnf/--stats-file/--rand; the script adds those per task).
#
#   --partition: the slurm queue. On vastlab: batch (4d max, 2d default),
#     debug (10h), xrtb/avedb (4d), xrtd/avedd (10h).
#   --task-time: per-task wall limit; omitted by default so the partition's
#     default applies (batch: 2 days).
#   --sbatch-opts: extra flags passed straight to sbatch, e.g.
#     "--qos=low --requeue". Requeue is safe: a task only counts as done once
#     its results/ file exists (or its log shows "Simulation is complete");
#     a requeued/preempted task detects its own partial log and reruns it.
#
#   Progress:  squeue -u $USER               Cancel: scancel -n sat-<folder>
#   Summary:   tools/run_slurm.sh --summarize runs/<folder>

set -u

# ---------------------------------------------------------------- worker mode
if [[ "${1:-}" == "--worker" ]]; then
    REPO=$2; RUNDIR=$3
    TASK_ID=${SLURM_ARRAY_TASK_ID:?not inside a slurm array task}
    cd "$REPO" || exit 1

    export PATH="$HOME/sst/local/sstcore-15.0.0/bin:$PATH"
    if [[ -z "${LD_PRELOAD:-}" && -f "$HOME/mimalloc/build/libmimalloc.so" ]]; then
        export LD_PRELOAD="$HOME/mimalloc/build/libmimalloc.so"
    fi

    task=$(sed -n "$((TASK_ID + 1))p" "$RUNDIR/tasks.txt")
    [[ -z "$task" ]] && { echo "no task at index $TASK_ID"; exit 1; }
    file=${task%%|*}
    seed=${task##*|}
    filename=$(basename "$file")

    seed_dir="$RUNDIR/seed$seed"
    mkdir -p "$seed_dir" "$RUNDIR/results"
    log_file="$seed_dir/${filename}.log"
    stats_file="$seed_dir/${filename}.stats.csv"
    result_file="$RUNDIR/results/${filename}.seed${seed}"

    # Resume/requeue safety: a run only counts as done once its result file
    # exists (written after classification) or its log reached completion.
    # A partial log left by a preempted/killed/requeued attempt is deleted
    # and the instance rerun.
    if [[ -f "$result_file" ]]; then
        exit 0
    fi
    if [[ -f "$log_file" ]]; then
        if grep -q "Simulation is complete" "$log_file"; then
            echo "$filename|SKIPPED|-|-" > "$result_file"
            exit 0
        fi
        echo "stale partial log (restart_count=${SLURM_RESTART_COUNT:-0}); rerunning"
        rm -f "$log_file" "$stats_file"
    fi

    mapfile -t sim_args < "$RUNDIR/simargs.txt"
    start_time=$(date +"%H:%M:%S")
    echo "host=$(hostname) task=$TASK_ID cnf=$filename seed=$seed"

    sst ./tests/test_two_level.py -- --cnf "$file" --stats-file "$stats_file" \
        --rand "$seed" "${sim_args[@]}" > "$log_file" 2>&1
    exit_status=$?

    if [[ -f "$stats_file" ]]; then
        echo -e "\nParsing statistics file: $stats_file" >> "$log_file"
        python3 ./tools/parse_stats.py "$stats_file" >> "$log_file" 2>&1
    else
        echo -e "\nWarning: Statistics file not found at $stats_file" >> "$log_file"
    fi

    # Verify solution for SAT verdicts (same as run_sc_l2.sh)
    verifier_status=1
    if [[ $exit_status -eq 0 ]] && grep -q "SATISFIABLE: All variables assigned" "$log_file"; then
        solution_file=$(mktemp)
        grep -A 2 "SATISFIABLE: All variables assigned" "$log_file" | grep -E 'x[0-9]+=' > "$solution_file"
        if [[ -s "$solution_file" ]]; then
            python3 ./tools/verifier.py "$solution_file" "$file" > /dev/null 2>&1
            verifier_status=$?
            if [[ $verifier_status -ne 0 ]]; then
                echo -e "\nVerification FAILED." >> "$log_file"
            else
                echo -e "\nVerification PASSED." >> "$log_file"
            fi
        fi
        rm -f "$solution_file"
    fi

    end_time=$(date +"%H:%M:%S")
    if [[ $exit_status -ne 0 ]]; then
        result="FAILED"
    elif grep -q "Timeout Reached" "$log_file"; then
        result="TIMEOUT"
    elif [[ $(grep -i -c -E "error|fault" "$log_file") -gt 0 ]]; then
        result="FAILED"
    elif grep -q "SATISFIABLE: All variables assigned" "$log_file"; then
        # SAT: verifier.py is authoritative (checks the model against the CNF)
        if [[ $verifier_status -eq 0 ]]; then
            result="PASSED_SAT"
        else
            result="FAILED_SAT"   # claimed SAT but model does not satisfy the CNF
        fi
    elif grep -q "UNSATISFIABLE" "$log_file"; then
        result="PASSED_UNSAT"     # trust the simulator's UNSAT verdict
    else
        result="FAILED"
    fi

    echo "$filename|$result|$start_time|$end_time" > "$result_file"
    echo "$filename seed=$seed: $result ($start_time-$end_time)"
    exit 0
fi

# ------------------------------------------------------------ summarize mode
if [[ "${1:-}" == "--summarize" ]]; then
    RUNDIR=${2:?usage: $0 --summarize runs/<folder>}
    [[ -d "$RUNDIR" ]] || { echo "no such run dir: $RUNDIR"; exit 1; }

    # elapsed <start> <end> (both HH:MM:SS) -> H:MM:SS (approximate; wraps at 24h)
    elapsed() {
        local s=$1 e=$2
        [[ -z "$s" || -z "$e" || "$s" == "-" || "$e" == "-" ]] && { echo "-"; return; }
        local ss=$(( 10#${s:0:2}*3600 + 10#${s:3:2}*60 + 10#${s:6:2} ))
        local es=$(( 10#${e:0:2}*3600 + 10#${e:3:2}*60 + 10#${e:6:2} ))
        local d=$(( es - ss )); (( d < 0 )) && d=$(( d + 86400 ))
        printf '%d:%02d:%02d' $(( d/3600 )) $(( (d%3600)/60 )) $(( d%60 ))
    }

    # OOM signature in a set of log/slurm files (kernel-OOM or SLURM cgroup)
    is_oom() { grep -qiE "out of memory|oom-kill|oom killer|std::bad_alloc|bad_alloc|Cannot allocate memory|MemoryError|[0-9]+ Killed +sst " "$@" 2>/dev/null; }

    # sacct terminal state per array index (authoritative for tasks SLURM killed).
    # tasks.txt line number (0-based) == the task's slurm array index.
    jid=$(cat "$RUNDIR/jobid" 2>/dev/null)
    # Older runs have no jobid file: infer the most recent array job id from the
    # slurm output filenames (<jobid>_<idx>.out) so the fallback grep still works.
    if [[ -z "$jid" ]]; then
        jid=$(ls -t "$RUNDIR"/slurm/*.out 2>/dev/null | head -1 | sed -nE 's#.*/([0-9]+)_[0-9]+\.out$#\1#p')
    fi
    declare -A SACCT_STATE
    if [[ -n "$jid" ]] && command -v sacct >/dev/null; then
        while IFS='|' read -r jf st _; do
            [[ "$jf" == *.* ]] && continue            # skip .batch/.extern steps
            idx=${jf##*_}
            [[ "$idx" =~ ^[0-9]+$ ]] || continue
            SACCT_STATE[$idx]=${st%% *}               # first token: TIMEOUT / OUT_OF_MEMORY / ...
        done < <(sacct -j "$jid" --format=JobID,State --parsable2 --noheader 2>/dev/null)
    fi

    passed=0; timedout=0; skipped=0; failed_oom=0; failed_other=0
    declare -A DONE      # "<name>.seed<seed>" seen in results/
    oom_lines=""
    : > "$RUNDIR/filter_pass.txt"

    shopt -s nullglob
    for f in "$RUNDIR"/results/*; do
        [[ -f "$f" ]] || continue
        base=$(basename "$f"); seed=${base##*.seed}; name=${base%.seed*}
        IFS='|' read -r rname verdict start end < "$f"
        DONE[$base]=1
        case "$verdict" in
            PASSED|PASSED_SAT|PASSED_UNSAT) passed=$((passed + 1)); echo "$rname" >> "$RUNDIR/filter_pass.txt" ;;
            TIMEOUT)                        timedout=$((timedout + 1)) ;;
            SKIPPED)                        skipped=$((skipped + 1)) ;;
            *)  # FAILED/FAILED_SAT: worker survived; check if the sim child was OOM-killed
                scan=("$RUNDIR/seed$seed/$name.log")
                # cgroup oom-kill notice and the shell's "NNN Killed sst" line land in
                # the task's slurm .out, not the per-test log: scan it too
                tline=$(grep -nF "/$name|$seed" "$RUNDIR/tasks.txt" 2>/dev/null | head -1 | cut -d: -f1)
                [[ -n "$tline" && -f "$RUNDIR/slurm/${jid}_$((tline - 1)).out" ]] && scan+=("$RUNDIR/slurm/${jid}_$((tline - 1)).out")
                if is_oom "${scan[@]}"; then
                    failed_oom=$((failed_oom + 1)); oom_lines+="  $name"$'\n'
                else
                    failed_other=$((failed_other + 1))
                fi
                ;;
        esac
    done

    # Missing = submitted tasks (tasks.txt) with no result file. Split by the
    # task's real terminal state (sacct, then a slurm-.out grep fallback).
    m_wall=0; m_oom=0; m_run=0; m_other=0
    wall_lines=""; run_lines=""; other_lines=""
    if [[ -f "$RUNDIR/tasks.txt" ]]; then
        li=-1
        while IFS='|' read -r tfile tseed; do
            li=$((li + 1))
            [[ -z "$tfile" ]] && continue
            tname=$(basename "$tfile")
            [[ -n "${DONE[${tname}.seed${tseed}]:-}" ]] && continue
            st=${SACCT_STATE[$li]:-}
            out="$RUNDIR/slurm/${jid}_${li}.out"
            if [[ -z "$st" && -f "$out" ]]; then       # fallback when sacct has no row
                if grep -qiE "DUE TO TIME LIMIT" "$out"; then st=TIMEOUT
                elif is_oom "$out"; then st=OUT_OF_MEMORY; fi
            fi
            case "$st" in
                TIMEOUT)             m_wall=$((m_wall + 1));  wall_lines+="  $tname"$'\n' ;;
                OUT_OF_MEMORY|OOM)   m_oom=$((m_oom + 1));    oom_lines+="  $tname"$'\n' ;;
                RUNNING|PENDING|REQUEUED|RESIZING|SUSPENDED|"") m_run=$((m_run + 1)); run_lines+="  $tname"$'\n' ;;
                *)                   m_other=$((m_other + 1)); other_lines+="  $tname ($st)"$'\n' ;;
            esac
        done < "$RUNDIR/tasks.txt"
    fi
    oom=$((m_oom + failed_oom))
    missing=$((m_wall + m_oom + m_run + m_other))

    # Per-list console cap (full lists always go to summary.log). Override with
    # SUMMARIZE_CAP=0 to print everything to the terminal too.
    CONSOLE_CAP=${SUMMARIZE_CAP:-20}
    emit_list() {  # <title> <body>  -> full to summary.log, capped on the console
        local title=$1 body=$2 n
        [[ -z "$body" ]] && return
        n=$(printf '%s' "$body" | grep -c '^')
        { echo ""; echo "$title ($n):"; printf '%s' "$body"; } >> "$RUNDIR/summary.log"
        echo ""; echo "$title ($n):"
        if (( CONSOLE_CAP > 0 && n > CONSOLE_CAP )); then
            printf '%s' "$body" | head -n "$CONSOLE_CAP"
            echo "  ... and $((n - CONSOLE_CAP)) more (see $RUNDIR/summary.log)"
        else
            printf '%s' "$body"
        fi
    }

    # ---- counts: console + head of summary.log ----
    {
        echo "Summary for $RUNDIR ($(date))"
        echo "  Passed:        $passed   (-> filter_pass.txt)"
        echo "  Cycle timeout: $timedout   (sim --timeout-cycles)"
        echo "  Wall timeout:  $m_wall   (SLURM --task-time)"
        echo "  OOM:           $oom   (killed for memory; $failed_oom left a partial log)"
        echo "  Failed:        $failed_other   (crash/error, not OOM)"
        echo "  Queued/other:  $((m_run + m_other))   (still running, or state unknown)"
        [[ $skipped -gt 0 ]] && echo "  Skipped:       $skipped"
        [[ -z "$jid" ]] && echo "  (note: no jobid saved for this run -> wall/OOM split relies on slurm .out grep only)"
    } | tee "$RUNDIR/summary.log"

    # ---- actionable lists: full to summary.log, capped on the console ----
    emit_list "Wall-timeout tests" "$wall_lines"
    emit_list "OOM tests" "$oom_lines"
    emit_list "Queued/running/unknown" "$run_lines$other_lines"

    # ---- full per-test detail: summary.log only (glob is name-sorted) ----
    {
        echo ""
        echo "==============================================="
        printf '%-13s %10s  %-19s  %s\n' "VERDICT" "ELAPSED" "START-END" "TEST"
        for f in "$RUNDIR"/results/*; do
            [[ -f "$f" ]] || continue
            IFS='|' read -r name verdict start end < "$f"
            printf '%-13s %10s  %-19s  %s\n' \
                "$verdict" "$(elapsed "$start" "$end")" "$start-$end" "$name"
        done
    } >> "$RUNDIR/summary.log"
    exit 0
fi

# --------------------------------------------------------------- submit mode
BENCHMARK_DIR=""
FOLDER_NAME=""
NUM_SEEDS=1
SPECIFIC_SEED=""
MAX_PARALLEL=64
TASK_MEM=6G
TASK_TIME=""          # empty = partition default (batch: 2 days)
PARTITION=batch
SBATCH_OPTS=""        # extra sbatch flags, e.g. "--qos=low --requeue"

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--bench-dir|--benchmark-dir) BENCHMARK_DIR=$2; shift 2 ;;
        --folder)       FOLDER_NAME=$2;  shift 2 ;;
        --seed)         SPECIFIC_SEED=$2; shift 2 ;;
        --num-seeds)    NUM_SEEDS=$2;    shift 2 ;;
        --max-parallel) MAX_PARALLEL=$2; shift 2 ;;
        --task-mem)     TASK_MEM=$2;     shift 2 ;;
        --task-time)    TASK_TIME=$2;    shift 2 ;;
        --partition)    PARTITION=$2;    shift 2 ;;
        --sbatch-opts)  SBATCH_OPTS=$2;  shift 2 ;;
        --) shift; break ;;
        *) echo "Unknown option: $1 (sim args go after --)"; exit 1 ;;
    esac
done

[[ -z "$BENCHMARK_DIR" || -z "$FOLDER_NAME" ]] && {
    echo "usage: $0 -b BENCH_DIR --folder NAME [--seed N | --num-seeds K]"
    echo "          [--max-parallel N] [--task-mem SZ] [--task-time T] [--partition P]"
    echo "          -- <args for tests/test_two_level.py>"
    exit 1
}
[[ -d "$BENCHMARK_DIR" ]] || { echo "Benchmark dir not found: $BENCHMARK_DIR"; exit 1; }
command -v sbatch >/dev/null || { echo "sbatch not found -- run this on the slurm login node"; exit 1; }

cd "$(dirname "$0")/.." || exit 1
REPO=$PWD
RUNDIR="$REPO/runs/$FOLDER_NAME"
mkdir -p "$RUNDIR" "$RUNDIR/slurm"

# Save sim args (one per line, read back verbatim by workers)
: > "$RUNDIR/simargs.txt"
for a in "$@"; do echo "$a" >> "$RUNDIR/simargs.txt"; done

# Flatten (file, seed) tasks, skipping ones that already have a log (resume)
: > "$RUNDIR/tasks.txt"
n_skipped=0
if [[ -n "$SPECIFIC_SEED" ]]; then seeds=("$SPECIFIC_SEED"); else seeds=($(seq 0 $((NUM_SEEDS - 1)))); fi
for file in "$BENCHMARK_DIR"/*; do
    [[ -f "$file" ]] || continue
    name=$(basename "$file")
    for seed in "${seeds[@]}"; do
        # Done = result file exists, or the log reached completion. Partial
        # logs (killed/preempted tasks) are resubmitted and rerun.
        if [[ -f "$RUNDIR/results/${name}.seed${seed}" ]] \
           || grep -qs "Simulation is complete" "$RUNDIR/seed$seed/${name}.log"; then
            n_skipped=$((n_skipped + 1))
        else
            echo "$file|$seed" >> "$RUNDIR/tasks.txt"
        fi
    done
done

N=$(wc -l < "$RUNDIR/tasks.txt")
echo "Benchmark dir: $BENCHMARK_DIR"
echo "Tasks to run:  $N  (skipped $n_skipped with existing logs)"
[[ $N -eq 0 ]] && { echo "Nothing to do."; exit 0; }

extra_opts=()
[[ -n "$TASK_TIME" ]] && extra_opts+=(--time="$TASK_TIME")
[[ -n "$SBATCH_OPTS" ]] && extra_opts+=($SBATCH_OPTS)  # intentional word-split

jobid=$(sbatch --parsable \
    --job-name="sat-$FOLDER_NAME" \
    --partition="$PARTITION" \
    --ntasks=1 --cpus-per-task=1 --mem="$TASK_MEM" \
    --array="0-$((N - 1))%$MAX_PARALLEL" \
    --output="$RUNDIR/slurm/%A_%a.out" \
    "${extra_opts[@]}" \
    "$REPO/tools/run_slurm.sh" --worker "$REPO" "$RUNDIR")

# Persist the array job id (and its tasks.txt line = array index) so --summarize
# can ask sacct/slurm for the true terminal state of tasks that left no result.
echo "$jobid" > "$RUNDIR/jobid"

echo "Submitted array job $jobid ($N tasks, max $MAX_PARALLEL concurrent, mem=$TASK_MEM, time=${TASK_TIME:-partition default}${SBATCH_OPTS:+, extra: $SBATCH_OPTS})"
echo "  watch:     squeue -j $jobid"
echo "  cancel:    scancel $jobid"
echo "  summary:   $0 --summarize runs/$FOLDER_NAME"
