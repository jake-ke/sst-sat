#!/bin/zsh

./tools/run_l1_tests.sh --l1-size "4KiB" --folder logs_4KiB -j 32
./tools/run_l1_tests.sh --l1-size "8KiB" --folder logs_8KiB -j 32
./tools/run_l1_tests.sh --l1-size "16KiB" --folder logs_16KiB -j 32
./tools/run_l1_tests.sh --l1-size "32KiB" --folder logs_32KiB -j 32
./tools/run_l1_tests.sh --l1-size "64KiB" --folder logs_64KiB -j 32
./tools/run_l1_tests.sh --l1-size "128KiB" --folder logs_128KiB -j 32
./tools/run_l1_tests.sh --l1-size "256KiB" --folder logs_256KiB -j 32
./tools/run_l1_tests.sh --l1-size "512KiB" --folder logs_512KiB -j 32
./tools/run_l1_tests.sh --l1-size "1MiB" --folder logs_1MiB -j 32
./tools/run_l1_tests.sh --l1-size "2MiB" --folder logs_2MiB -j 32
./tools/run_l1_tests.sh --l1-size "4MiB" --folder logs_4MiB -j 32