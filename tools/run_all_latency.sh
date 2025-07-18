#!/bin/zsh

# varied l1 cache latency, mem latency = 100ns
./tools/run_l1_tests.sh --l1-latency "1" --folder logs_l1_1ns_mem_100ns -j 32
./tools/run_l1_tests.sh --l1-latency "5" --folder logs_l1_5ns_mem_100ns -j 32
./tools/run_l1_tests.sh --l1-latency "10" --folder logs_l1_10ns_mem_100ns -j 32
./tools/run_l1_tests.sh --l1-latency "30" --folder logs_l1_30ns_mem_100ns -j 32
./tools/run_l1_tests.sh --l1-latency "50" --folder logs_l1_50ns_mem_100ns -j 32
./tools/run_l1_tests.sh --l1-latency "100" --folder logs_l1_100ns_mem_100ns -j 32

# varied memory latency, l1 latency = 1ns
./tools/run_l1_tests.sh --mem-latency "50ns" --folder logs_l1_1ns_mem_50ns -j 32
./tools/run_l1_tests.sh --mem-latency "60ns" --folder logs_l1_1ns_mem_60ns -j 32
./tools/run_l1_tests.sh --mem-latency "70ns" --folder logs_l1_1ns_mem_70ns -j 32
./tools/run_l1_tests.sh --mem-latency "80ns" --folder logs_l1_1ns_mem_80ns -j 32
./tools/run_l1_tests.sh --mem-latency "90ns" --folder logs_l1_1ns_mem_90ns -j 32