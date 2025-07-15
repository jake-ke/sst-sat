#!/bin/zsh

./tools/run_l1_tests.sh "8KiB" logs_8KiB -j 32
./tools/run_l1_tests.sh "16KiB" logs_16KiB -j 32
./tools/run_l1_tests.sh "32KiB" logs_32KiB -j 32
./tools/run_l1_tests.sh "64KiB" logs_64KiB -j 32
./tools/run_l1_tests.sh "128KiB" logs_128KiB -j 32
./tools/run_l1_tests.sh "256KiB" logs_256KiB -j 32
./tools/run_l1_tests.sh "512KiB" logs_512KiB -j 32
./tools/run_l1_tests.sh "1MiB" logs_1MiB -j 32
./tools/run_l1_tests.sh "2MiB" logs_2MiB -j 32
./tools/run_l1_tests.sh "4MiB" logs_4MiB -j 32