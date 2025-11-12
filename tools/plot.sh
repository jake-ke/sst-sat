
DATA_DIR=$1

python3 tools/plot_prop_histogram.py $DATA_DIR/base_128KB/base_l1_4_1_l2_8_32/ results/prop_histogram.pdf

python3 tools/plot_breakdown.py $DATA_DIR/base_128KB/profile_base_l1_4_1_l2_8_32/seed0/ results/runtime_breakdown.pdf

python3 tools/plot_l1_miss_rate.py $DATA_DIR base_l1_4_1_l2_8_32/ results/l1_miss_rate.pdf

python3 tools/plot_l2_sweep.py $DATA_DIR/base_128KB base_l1_4_1 results/l2_sweep.pdf
