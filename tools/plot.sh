
DATA_DIR=../sat-isca26-data

python3 tools/plot_prop_histogram.py ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/ results/prop_histogram.pdf
cp results/prop_histogram_weighted.pdf ../sat-micro26/charts/prop_histogram.pdf

python3 tools/plot_breakdown.py ../sat-isca26-data/base_128KB/profile_base_l1_4_1_l2_8_32/seed0/ results/runtime_breakdown.pdf --accel ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --folder ../sat-isca26-data/incr3/opt2_l1_4_1_l2_8_32/seed3/ "Parallel Prop"
cp results/runtime_breakdown.pdf ../sat-micro26/charts/
# cp results/runtime_breakdown_combined.pdf ../sat-micro26/charts/

# donut breakdown
python3 tools/plot_pie_breakdown.py ../sat-isca26-data/base_128KB/profile_base_l1_4_1_l2_8_32/seed0/ results/donut_breakdown.pdf --accel ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --folder ../sat-isca26-data/incr3/opt2_l1_4_1_l2_8_32/seed3/ "+Prop" --donut
# cp results/breakdown_donut.pdf ../sat-micro26/charts/runtime_breakdown_combined.pdf
cp results/breakdown_pie.pdf ../sat-micro26/charts/runtime_breakdown_combined.pdf

python3 tools/plot_l1_miss_rate.py ../sat-isca26-data base_l1_4_1_l2_8_32/ results/l1_miss_rate.pdf
cp results/l1_miss_rate.pdf ../sat-micro26/charts/

python3 tools/plot_l2_sweep.py ../sat-isca26-data/ results/l2_sweep.pdf --timeout 36
cp results/l2_sweep_bw.pdf ../sat-micro26/charts/
cp results/l2_sweep_lat.pdf ../sat-micro26/charts/
# python3 tools/plot_l2_sweep.py --bw-dir ../sat-isca26-data/bw --bw-prefix base_l1_-1_1 --lat-dir ../sat-isca26-data/base_128KB --lat-prefix base_l1_4_1 results/l2_sweep.pdf


# large tests per-seed (uses hardcoded MANUAL_EXCLUSIVE_TESTS in plot_large_tests.py)
python3 tools/plot_large_tests.py --output-dir results/large_tests/ ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ../sat-isca26-data/stereo_minisat_logs/ ../sat-isca26-data/stereo_kissat_logs/ ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "MiniSAT" "Kissat" "SATBlast"
# python3 tools/plot_large_tests.py --output-dir results/large_tests/ ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ../sat-isca26-data/kilia_minisat_logs/ ../sat-isca26-data/kilia_kissat_logs/ ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "MiniSAT" "Kissat" "SATBlast"
cp results/large_tests/per_test_speedups.pdf ../sat-micro26/charts/large_tests.pdf


#### Comprehensive comparison of baseline, SATAccel, MiniSAT, Kissat, and SATBlast
# for stereo Intel i13900k
python3 tools/plot_comparison.py --output-dir results/ ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ~/openhw-2025-SAT-FPGA/results.txt ../sat-isca26-data/stereo_minisat_logs/ ../sat-isca26-data/stereo_kissat_logs/ ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "SAT-Accel" "MiniSAT" "Kissat" "SATBlast" --large-fonts

# for kilia Ryzen 9 9950X3D
# python3 tools/plot_comparison.py --output-dir results/ ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ../sat-isca26-data/kilia_minisat_logs/ ../sat-isca26-data/kilia_kissat_logs/ ~/openhw-2025-SAT-FPGA/results.txt ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "MiniSAT" "Kissat" "SAT-Accel" "SATBlast" --large-fonts
cp results/comparison_charts.pdf ../sat-micro26/charts/par2.pdf
cp results/geomean_speedups.pdf ../sat-micro26/charts/gmean.pdf
cp results/cactus_plot.pdf ../sat-micro26/charts/

# no timeouts par-2 geomean
python3 tools/plot_comparison.py --output-dir results/no_timeouts/ ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ~/openhw-2025-SAT-FPGA/results.txt ../sat-isca26-data/stereo_minisat_logs/ ../sat-isca26-data/stereo_kissat_logs/ ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "SAT-Accel" "MiniSAT" "Kissat" "SATBlast" --exclude-timeouts-geomean  --large-fonts
# python3 tools/plot_comparison.py --output-dir results/no_timeouts ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ../sat-isca26-data/kilia_minisat_logs/ ../sat-isca26-data/kilia_kissat_logs/ ~/openhw-2025-SAT-FPGA/results.txt ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "MiniSAT" "Kissat" "SAT-Accel" "SATBlast" --exclude-timeouts-geomean  --large-fonts
cp results/no_timeouts/comparison_charts.pdf ../sat-micro26/charts/no_timeout_avg.pdf
cp results/no_timeouts/geomean_speedups.pdf ../sat-micro26/charts/no_timeout_gmean.pdf


# incr
python3 tools/plot_comparison.py --output-dir results/incr/ ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3  ../sat-isca26-data/incr3/opt2_l1_4_1_l2_8_32/ ../sat-isca26-data/incr3/opt3-new_l1_4_1_l2_8_32/ ../sat-isca26-data/incr3/opt4-new_l1_4_1_l2_8_32/ ../sat-isca26-data/incr3/opt5_l1_4_1_l2_8_32/ ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "+Prop" "+Heap" "+Learn" "+WL" "+Prefetch/Min" --errors-as-timeout --line  --highlight-last "SATBlast"
cp results/incr/geomean_speedups.pdf ../sat-micro26/charts/incr_gmean.pdf

# lits
## NEED TO FIX SOME did not finish tests!
# Lits-12, 1427381a809c64c721838894ece6756d-shuffling-2-s25242449-of-bench-sat04-727.used-as.sat04-753         
# Lits-16, 43e492bccfd57029b758897b17d7f04f-pb_300_09_lb_07
# add --errors-as-timeout back after fix
python3 tools/plot_comparison.py --output-dir results/lits/ ../sat-isca26-data/lits3/lits-1/ ../sat-isca26-data/lits3/lits-4/seed3 ~/sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3 ../sat-isca26-data/lits3/lits-16/seed3 --names "Lits-1" "Lits-4" "Lits-8" "Lits-16" --line --errors-as-timeout --large-fonts
# kept lit-12
# python3 tools/plot_comparison.py --output-dir results/lits/ ../sat-isca26-data/lits3/lits-4/ ~/sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3 ../sat-isca26-data/lits3/lits-12/ ../sat-isca26-data/lits3/lits-16/ --names "Lits-4" "Lits-8" "Lits-12" "Lits-16" --line --errors-as-timeout
cp results/lits/geomean_speedups.pdf ../sat-micro26/charts/lits_gmean.pdf

# confl
## NEED TO FIX SOME did not finish tests!
# Confl-4, 1427381a809c64c721838894ece6756d-shuffling-2-s25242449-of-bench-sat04-727.used-as.sat04-753         
# Confl-4, fce130da1efd25fa9b12e59620a4146b-g2-ak128diagobg1btaig  
# add --errors-as-timeout back after fix
python3 tools/plot_comparison.py --output-dir results/confl/ ../sat-isca26-data/confl3/confl1/ ../sat-isca26-data/confl3/confl4/ ~/sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3 ../sat-isca26-data/confl3/confl12/  --names "Confl-1" "Confl-4" "Confl-8" "Confl-12" --errors-as-timeout --line
# confl8 is basically opt-final but added profiling?
# python3 tools/plot_comparison.py --output-dir results/confl/ ../sat-isca26-data/confl3/confl1/ ../sat-isca26-data/confl3/confl4/ ~/sat-isca26-data/confl3/confl8/ ../sat-isca26-data/confl3/confl12/  --names "Confl-1" "Confl-4" "Confl-8" "Confl-12" --line --errors-as-timeout
cp results/confl/geomean_speedups.pdf ../sat-micro26/charts/confl_gmean.pdf

# scaled 2MB
python3 tools/plot_comparison.py --output-dir results/scale_2mb/ ../sat-isca26-data/scaled3/base_l1_4_1_l2_8_32/ ../sat-isca26-data/scaled3/opt_l1_4_1_l2_8_32/ --names "Baseline+" "SATBlast+" --errors-as-timeout --large-fonts
# python3 tools/plot_comparison.py --output-dir results/scale_2mb/ ../sat-isca26-data/scaled_2MB/base_l1_4_1_l2_8_32/ ../sat-isca26-data/scaled_2MB/opt_l1_4_1_l2_8_32/ --names "Baseline+" "SATBlast+"
cp results/scale_2mb/comparison_charts.pdf ../sat-micro26/charts/scale_par2.pdf
cp results/scale_2mb/geomean_speedups.pdf ../sat-micro26/charts/scale_gmean.pdf



# cache
python3 tools/plot_cache_comparison.py --output-dir results/ ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "SATBlast"
cp results/cache_comparison.pdf ../sat-micro26/charts/


#################### rebuttal

# roofline, throughput, bandwidth comparison
python3 tools/plot_throughput_roofline.py ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "SATBlast" --peak-bandwidth 512 --peak-compute 4.81e8 --output-dir results/
# cp results/roofline.pdf ../sat-micro26/charts/
# cp results/throughput_vs_req_per_prop.pdf ../sat-micro26/charts/
cp results/req_per_prop.pdf ../sat-micro26/charts/
cp results/bandwidth_comparison.pdf ../sat-micro26/charts/
# cp results/propagations_per_sec.pdf ../sat-micro26/charts/

# multi-learn profiling
python3 tools/plot_learning_comparison.py --output-dir results/learning/ ../sat-isca26-data/confl3/confl1/ ../sat-isca26-data/confl3/confl4/ ~/sat-isca26-data/confl3/confl8/ ../sat-isca26-data/confl3/confl12/  --names "Confl-1" "Confl-4" "Confl-8" "Confl-12"
cp results/learning/learning_geomean_ratio.pdf ../sat-micro26/charts/

# comprehensive comparison of baseline, SATAccel, MiniSAT, Kissat, and SATBlast
# normalize SATAccel by assuming it runs 4x faster frequency
# add  --normalize-sataccel
python3 tools/plot_comparison.py --output-dir results/ ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ~/openhw-2025-SAT-FPGA/results.txt ../sat-isca26-data/stereo_minisat_logs/ ../sat-isca26-data/stereo_kissat_logs/ ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "SAT-Accel" "MiniSAT" "Kissat" "SATBlast" --large-fonts --normalize-sataccel
cp results/comparison_charts.pdf ../sat-micro26/charts/par2.pdf
cp results/geomean_speedups.pdf ../sat-micro26/charts/gmean.pdf
cp results/cactus_plot.pdf ../sat-micro26/charts/
# no timeout
python3 tools/plot_comparison.py --output-dir results/no_timeouts/ ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ~/openhw-2025-SAT-FPGA/results.txt ../sat-isca26-data/stereo_minisat_logs/ ../sat-isca26-data/stereo_kissat_logs/ ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "SAT-Accel" "MiniSAT" "Kissat" "SATBlast" --exclude-timeouts-geomean --large-fonts --normalize-sataccel
cp results/no_timeouts/comparison_charts.pdf ../sat-micro26/charts/no_timeout_avg.pdf
cp results/no_timeouts/geomean_speedups.pdf ../sat-micro26/charts/no_timeout_gmean.pdf

# Comparing SATBlast at 250MHz
python3 tools/plot_comparison.py --output-dir results/ ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ~/openhw-2025-SAT-FPGA/results.txt ../sat-isca26-data/stereo_minisat_logs/ ../sat-isca26-data/stereo_kissat_logs/ ../sat-isca26-data/opt-final250MHz_nospec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "SAT-Accel" "MiniSAT" "Kissat" "SATBlast" --large-fonts


# case study of coproc
python3 tools/plot_coproc_comparison.py ~/sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ ~/scratch/coproc.154830/sst-sat/runs/opt-final_l1_4_1_l2_8_32/seed3/ ~/scratch/lbd-coproc.154831/sst-sat/runs/lbd-opt-final_l1_4_1_l2_8_32/seed3/
# with SATBlast+LBD
# python3 tools/plot_coproc_comparison.py ~/sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ ~/scratch/lbd-opt-final.154597/sst-sat/runs/lbd-opt-final_l1_4_1_l2_8_32/seed3/ ~/scratch/coproc.154830/sst-sat/runs/opt-final_l1_4_1_l2_8_32/seed3/ ~/scratch/lbd-coproc.154831/sst-sat/runs/lbd-opt-final_l1_4_1_l2_8_32/seed3/
cp results/coproc_comparison.pdf ../sat-micro26/charts/copro_case_study.pdf


# new large tests bar for exclusive tests only
python3 tools/plot_large_tests.py --output-dir results/large_tests/ ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ../sat-isca26-data/stereo_minisat_logs/ ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ --names "Baseline" "MiniSAT" "SATBlast"
cp results/large_tests/per_test_speedups.pdf ../sat-micro26/charts/large_tests.pdf
# large tests scatter
# use multiple seeds better
python3 tools/plot_speedup_scatter.py ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/ ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/ --names "Baseline" "SATBlast" --output-dir results/ --exclude-timeouts
cp results/speedup_vs_runtime.pdf ../sat-micro26/charts/large_tests.pdf


# combined lits and confl ablation plots
python3 tools/plot_ablation.py --output-dir results/ablation/
cp results/ablation/ablation_combined.pdf ../sat-micro26/charts/


# combined overall perf
python3 tools/plot_overall_perf.py --output-dir results/ ../sat-isca26-data/base_128KB/base_l1_4_1_l2_8_32/seed3 ~/openhw-2025-SAT-FPGA/results.txt ../sat-isca26-data/stereo_minisat_logs/ ../sat-isca26-data/stereo_kissat_logs/ ../sat-isca26-data/opt_128KB_no-spec_l1_4_1_l2_8_32/seed3/ ~/scratch/lbd-coproc.154831/sst-sat/runs/lbd-opt-final_l1_4_1_l2_8_32/seed3/ --names "Baseline" "SAT-Accel" "MiniSAT" "Kissat" "SATBlast" "SATBlast+LBD" --normalize-sataccel
cp results/overall_perf.pdf ../sat-micro26/charts/perf_comb.pdf
cp results/overall_perf_no_timeouts.pdf ../sat-micro26/charts/perf_comb_no_timeouts.pdf

