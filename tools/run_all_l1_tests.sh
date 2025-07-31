#!/bin/zsh

# ./tools/run_l1_tests.sh --l1-size "4KiB" --folder logs_4KiB -j 32
# ./tools/run_l1_tests.sh --l1-size "8KiB" --folder logs_8KiB -j 32
# ./tools/run_l1_tests.sh --l1-size "16KiB" --folder logs_16KiB -j 32
# ./tools/run_l1_tests.sh --l1-size "32KiB" --folder logs_32KiB -j 32
# ./tools/run_l1_tests.sh --l1-size "64KiB" --folder logs_64KiB -j 32
# ./tools/run_l1_tests.sh --l1-size "128KiB" --folder logs_128KiB -j 32
# ./tools/run_l1_tests.sh --l1-size "256KiB" --folder logs_256KiB -j 32
# ./tools/run_l1_tests.sh --l1-size "512KiB" --folder logs_512KiB -j 32
# ./tools/run_l1_tests.sh --l1-size "1MiB" --folder logs_1MiB -j 32
# ./tools/run_l1_tests.sh --l1-size "2MiB" --folder logs_2MiB -j 32
# ./tools/run_l1_tests.sh --l1-size "4MiB" --folder logs_4MiB -j 32

# Ramulator2 DDR4 configuration
./tools/run_l1_tests.sh --ram2-cfg tests/ramulator2-ddr4.cfg --l1-size "4KiB" --folder logs_ddr_4KiB -j 32
./tools/run_l1_tests.sh --ram2-cfg tests/ramulator2-ddr4.cfg --l1-size "8KiB" --folder logs_ddr_8KiB -j 32
./tools/run_l1_tests.sh --ram2-cfg tests/ramulator2-ddr4.cfg --l1-size "16KiB" --folder logs_ddr_16KiB -j 32
./tools/run_l1_tests.sh --ram2-cfg tests/ramulator2-ddr4.cfg --l1-size "32KiB" --folder logs_ddr_32KiB -j 32
./tools/run_l1_tests.sh --ram2-cfg tests/ramulator2-ddr4.cfg --l1-size "64KiB" --folder logs_ddr_64KiB -j 32
./tools/run_l1_tests.sh --ram2-cfg tests/ramulator2-ddr4.cfg --l1-size "128KiB" --folder logs_ddr_128KiB -j 32
./tools/run_l1_tests.sh --ram2-cfg tests/ramulator2-ddr4.cfg --l1-size "256KiB" --folder logs_ddr_256KiB -j 32
./tools/run_l1_tests.sh --ram2-cfg tests/ramulator2-ddr4.cfg --l1-size "512KiB" --folder logs_ddr_512KiB -j 32
./tools/run_l1_tests.sh --ram2-cfg tests/ramulator2-ddr4.cfg --l1-size "1MiB" --folder logs_ddr_1MiB -j 32
./tools/run_l1_tests.sh --ram2-cfg tests/ramulator2-ddr4.cfg --l1-size "2MiB" --folder logs_ddr_2MiB -j 32
./tools/run_l1_tests.sh --ram2-cfg tests/ramulator2-ddr4.cfg --l1-size "4MiB" --folder logs_ddr_4MiB -j 32


# # varied l1 cache latency, mem latency = 100ns
# ./tools/run_l1_tests.sh --l1-latency "1" --folder logs_l1_1ns_mem_100ns -j 32
# ./tools/run_l1_tests.sh --l1-latency "5" --folder logs_l1_5ns_mem_100ns -j 32
# ./tools/run_l1_tests.sh --l1-latency "10" --folder logs_l1_10ns_mem_100ns -j 32
# ./tools/run_l1_tests.sh --l1-latency "30" --folder logs_l1_30ns_mem_100ns -j 32
# ./tools/run_l1_tests.sh --l1-latency "50" --folder logs_l1_50ns_mem_100ns -j 32
# ./tools/run_l1_tests.sh --l1-latency "100" --folder logs_l1_100ns_mem_100ns -j 32

# # varied memory latency, l1 latency = 1ns
# ./tools/run_l1_tests.sh --mem-latency "50ns" --folder logs_l1_1ns_mem_50ns -j 32
# ./tools/run_l1_tests.sh --mem-latency "60ns" --folder logs_l1_1ns_mem_60ns -j 32
# ./tools/run_l1_tests.sh --mem-latency "70ns" --folder logs_l1_1ns_mem_70ns -j 32
# ./tools/run_l1_tests.sh --mem-latency "80ns" --folder logs_l1_1ns_mem_80ns -j 32
# ./tools/run_l1_tests.sh --mem-latency "90ns" --folder logs_l1_1ns_mem_90ns -j 32

# ramulator2 DDR4 configuration with varied latencies
./tools/run_l1_tests.sh --ram2-cfg tests/ramulator2-ddr4.cfg --l1-latency "1" --folder logs_ddr_l1_1ns_mem_100ns -j 32
./tools/run_l1_tests.sh --ram2-cfg tests/ramulator2-ddr4.cfg --l1-latency "5" --folder logs_ddr_l1_5ns_mem_100ns -j 32
./tools/run_l1_tests.sh --ram2-cfg tests/ramulator2-ddr4.cfg --l1-latency "10" --folder logs_ddr_l1_10ns_mem_100ns -j 32
./tools/run_l1_tests.sh --ram2-cfg tests/ramulator2-ddr4.cfg --l1-latency "30" --folder logs_ddr_l1_30ns_mem_100ns -j 32
./tools/run_l1_tests.sh --ram2-cfg tests/ramulator2-ddr4.cfg --l1-latency "50" --folder logs_ddr_l1_50ns_mem_100ns -j 32
./tools/run_l1_tests.sh --ram2-cfg tests/ramulator2-ddr4.cfg --l1-latency "100" --folder logs_ddr_l1_100ns_mem_100ns -j 32
