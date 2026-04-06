#!/bin/bash

OUTDIR="/home/jakeke/tmp3/runs/opt-final_satlib/seed3"
CNFDIR="$HOME/sat_benchmarks/satlib"
COMMON="--ram2-cfg tests/ramulator2-ddr4.cfg --l1-size 128KiB --l1-latency 1 --l1-bw 4 --l2-latency 32 --l2-bw 8 --prefetch --rand 3 --timeout-cycles 36000000000"

FAILED=(
  aim-200-3_4-yes1-4.cnf
  aim-200-1_6-no-4.cnf
  CBS_k3_n100_m403_b10_1.cnf
)

cd /home/jakeke/tmp3

passed=0
failed=0
for cnf in "${FAILED[@]}"; do
  echo "=== Running $cnf ==="
  statsfile="${OUTDIR}/${cnf}.stats.csv"
  logfile="${OUTDIR}/${cnf}.log"
  
  sst ./tests/test_two_level.py -- \
    --ram2-cfg tests/ramulator2-ddr4.cfg \
    --cnf "${CNFDIR}/${cnf}" \
    $COMMON \
    --stats-file "$statsfile" \
    |& tee "$logfile"
  
  if grep -q "Simulation is complete" "$logfile"; then
    echo ">>> PASSED: $cnf"
    passed=$((passed + 1))
  else
    echo ">>> FAILED: $cnf"
    failed=$((failed + 1))
  fi
  echo ""
done

echo "================================"
echo "Results: $passed passed, $failed failed out of ${#FAILED[@]}"
echo "================================"
