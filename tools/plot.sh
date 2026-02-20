
DATA_DIR=../sat-isca26-data

python3 tools/plot_prop_histogram.py ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/ results/prop_histogram.pdf
cp results/prop_histogram.pdf ../sat-isca2026/charts/

python3 tools/plot_breakdown.py ../sat-isca26-data/base_128KB/profile_base_l1_4_1_l2_8_32/seed0/ results/runtime_breakdown.pdf

python3 tools/plot_l1_miss_rate.py ../sat-isca26-data base_l1_4_1_l2_8_32/ results/l1_miss_rate.pdf
cp results/l1_miss_rate.pdf ../sat-isca2026/charts/

# python3 tools/plot_l2_sweep.py ../sat-isca26-data/base_128KB base_l1_4_1 results/l2_sweep.pdf --timeout 36
python3 tools/plot_l2_sweep.py --bw-dir ../sat-isca26-data/bw --bw-prefix base_l1_-1_1 --lat-dir ../sat-isca26-data/base_128KB --lat-prefix base_l1_4_1 results/l2_sweep.pdf



# MUST!!!! specify MANUAL_EXCLUSIVE_TESTS in plot_comparison.py for large tests - no sat accel results
# large tests per-seed
python3 tools/plot_comparison.py --output-dir results/large_tests/ ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ../sat-isca26-data/stereo_minisat_logs/ ../sat-isca26-data/stereo_kissat_logs/ ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "MiniSAT" "Kissat" "SATBlast"
cp results/large_tests/per_test_speedups.pdf ../sat-isca2026/charts/large_tests.pdf
# below this, no MANUAL_EXCLUSIVE_TESTS

# same as above but including sat accel
python3 tools/plot_comparison.py --output-dir results/ ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ~/openhw-2025-SAT-FPGA/results.txt ../sat-isca26-data/stereo_minisat_logs/ ../sat-isca26-data/stereo_kissat_logs/ ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "SATAccel" "MiniSAT" "Kissat" "SATBlast" --large-fonts
cp results/comparison_charts.pdf ../sat-isca2026/charts/par2.pdf
cp results/geomean_speedups.pdf ../sat-isca2026/charts/gmean.pdf
cp results/cactus_plot.pdf ../sat-isca2026/charts/

# no timeouts par-2 geomean
python3 tools/plot_comparison.py --output-dir results/no_timeouts/ ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ~/openhw-2025-SAT-FPGA/results.txt ../sat-isca26-data/stereo_minisat_logs/ ../sat-isca26-data/stereo_kissat_logs/ ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "SATAccel" "MiniSAT" "Kissat" "SATBlast" --exclude-timeouts-geomean  --large-fonts
cp results/no_timeouts/comparison_charts.pdf ../sat-isca2026/charts/no_timeout_avg.pdf
cp results/no_timeouts/geomean_speedups.pdf ../sat-isca2026/charts/no_timeout_gmean.pdf


# incr
python3 tools/plot_comparison.py --output-dir results/incr/ ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ../sat-isca26-data/incr3/opt1_l1_4_1_l2_8_32/  ../sat-isca26-data/incr3/opt2_l1_4_1_l2_8_32/ ../sat-isca26-data/incr3/opt3-new_l1_4_1_l2_8_32/ ../sat-isca26-data/incr3/opt4-new_l1_4_1_l2_8_32/ ../sat-isca26-data/incr3/opt5_l1_4_1_l2_8_32/ ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "Lit" "Wat" "Heap" "Learn" "WL" "SATBlast" --errors-as-timeout
cp results/incr/geomean_speedups.pdf ../sat-isca2026/charts/incr_gmean.pdf

# lits
python3 tools/plot_comparison.py --output-dir results/lits/ ../sat-isca26-data/lits3/lits-4/ ~/sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3 ../sat-isca26-data/lits3/lits-12/ ../sat-isca26-data/lits3/lits-16/ --names "Lits-4" "SATBlast" "Lits-12" "Lits-16" --errors-as-timeout
cp results/lits/geomean_speedups.pdf ../sat-isca2026/charts/lits_gmean.pdf

# confl
python3 tools/plot_comparison.py --output-dir results/confl/ ../sat-isca26-data/confl3/confl1/ ../sat-isca26-data/confl3/confl4/ ~/sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3 ../sat-isca26-data/confl3/confl12/  --names "Confl-1" "Confl-4" "SATBlast" "Confl-12" --errors-as-timeout
cp results/confl/geomean_speedups.pdf ../sat-isca2026/charts/confl_gmean.pdf

# scaled 2MB
python3 tools/plot_comparison.py --output-dir results/scale_2mb/ ../sat-isca26-data/scaled3/base_l1_4_1_l2_8_32/ ../sat-isca26-data/scaled3/opt_l1_4_1_l2_8_32/ --names "Baseline+" "SATBlast+" --errors-as-timeout --large-fonts
# python3 tools/plot_comparison.py --output-dir results/scale_2mb/ ../sat-isca26-data/scaled_2MB/base_l1_4_1_l2_8_32/ ../sat-isca26-data/scaled_2MB/opt_l1_4_1_l2_8_32/ --names "Baseline+" "SATBlast+"
cp results/scale_2mb/comparison_charts.pdf ../sat-isca2026/charts/scale_par2.pdf
cp results/scale_2mb/geomean_speedups.pdf ../sat-isca2026/charts/scale_gmean.pdf



# cache
python3 tools/plot_cache_comparison.py --output-dir results/ ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "SATBlast"
cp results/cache_comparison.pdf ../sat-isca2026/charts/
