#!/bin/bash
# Rerun hole* benchmarks with 30 seeds, find best average runtime
# Timeout: 400ms (400000000 cycles)

BENCHDIR="$HOME/sat_benchmarks/satlib"
SEEDS=20
TIMEOUT=36000000000
PARALLEL=20  # max parallel jobs
GLUCOSE=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --glucose-restart) GLUCOSE="--glucose-restart"; shift ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

if [ -n "$GLUCOSE" ]; then
    OUTDIR="runs/hole_seed_sweep_glucose"
else
    OUTDIR="runs/hole_seed_sweep"
fi

mkdir -p "$OUTDIR"

HOLES=(hole10)
# HOLES=(hole6 hole7 hole8 hole9 hole10)

# Run all tests
for hole in "${HOLES[@]}"; do
    cnf="$BENCHDIR/${hole}.cnf"
    holedir="$OUTDIR/$hole"
    mkdir -p "$holedir"

    for seed in $(seq 0 $((SEEDS - 1))); do
        logfile="$holedir/seed${seed}.log"
        # Skip if already completed
        if [ -f "$logfile" ] && grep -q "simulated time\|TIMEOUT" "$logfile" 2>/dev/null; then
            continue
        fi
        # Throttle parallel jobs
        while [ "$(jobs -rp | wc -l)" -ge "$PARALLEL" ]; do
            sleep 0.5
        done
        echo "Running $hole seed=$seed ..."
        sst ./tests/test_two_level.py -- \
            --ram2-cfg tests/ramulator2-ddr4.cfg \
            --cnf "$cnf" \
            --l1-size 128KiB --l1-latency 1 --l1-bw 4 \
            --l2-latency 32 --l2-bw 8 \
            --prefetch --rand "$seed" \
            --timeout-cycles "$TIMEOUT" $GLUCOSE \
            > "$logfile" 2>&1 &
    done
done

# Wait for all jobs
wait
echo ""
echo "All runs complete. Parsing results..."
echo ""

# Parse results and compute averages
printf "%-8s" "Seed"
for hole in "${HOLES[@]}"; do
    printf "%12s" "$hole"
done
printf "%12s\n" "Average"
printf '%0.s-' {1..80}; echo ""

best_avg=""
best_seed=""

for seed in $(seq 0 $((SEEDS - 1))); do
    printf "%-8s" "$seed"
    total=0
    count=0
    all_valid=true

    for hole in "${HOLES[@]}"; do
        logfile="$OUTDIR/$hole/seed${seed}.log"
        # Extract simulated time in ms
        time_ms=$(grep "simulated time:" "$logfile" 2>/dev/null | grep -oP '[\d.]+(?= ms)')
        if grep -q "Timeout Reached" "$logfile" 2>/dev/null || [ -z "$time_ms" ]; then
            printf "%12s" "TIMEOUT"
            all_valid=false
        else
            printf "%11.1fms" "$time_ms"
            total=$(echo "$total + $time_ms" | bc -l)
            count=$((count + 1))
        fi
    done

    if [ "$count" -gt 0 ]; then
        avg=$(echo "$total / ${#HOLES[@]}" | bc -l)
        printf "%11.1fms" "$avg"
        # Track best (lowest) average — only if all completed
        if $all_valid; then
            if [ -z "$best_avg" ] || [ "$(echo "$avg < $best_avg" | bc -l)" -eq 1 ]; then
                best_avg="$avg"
                best_seed="$seed"
            fi
        fi
    else
        printf "%12s" "N/A"
    fi
    echo ""
done

echo ""
printf '%0.s-' {1..80}; echo ""
if [ -n "$best_seed" ]; then
    printf "Best seed: %d (avg: %.1fms)\n" "$best_seed" "$best_avg"
else
    echo "No seed completed all benchmarks without timeout."
    # Find best partial average
    echo "Finding best partial average..."
    best_partial=""
    best_partial_seed=""
    for seed in $(seq 0 $((SEEDS - 1))); do
        total=0; count=0
        for hole in "${HOLES[@]}"; do
            logfile="$OUTDIR/$hole/seed${seed}.log"
            time_ms=$(grep "simulated time:" "$logfile" 2>/dev/null | grep -oP '[\d.]+(?= ms)')
            if [ -n "$time_ms" ] && ! grep -q "Timeout Reached" "$logfile" 2>/dev/null; then
                total=$(echo "$total + $time_ms" | bc -l)
                count=$((count + 1))
            fi
        done
        if [ "$count" -gt 0 ]; then
            avg=$(echo "$total / $count" | bc -l)
            if [ -z "$best_partial" ] || [ "$(echo "$avg < $best_partial" | bc -l)" -eq 1 ]; then
                best_partial="$avg"
                best_partial_seed="$seed"
            fi
        fi
    done
    if [ -n "$best_partial_seed" ]; then
        printf "Best partial avg: seed %d (%.1fms over completed tests)\n" "$best_partial_seed" "$best_partial"
    fi
fi
