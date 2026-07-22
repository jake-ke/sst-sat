import argparse
import os

import sst

# Parse command line arguments
parser = argparse.ArgumentParser(description='Run the PipelinedHeap manual verification test')
parser.add_argument('--verbose', type=int, default=1,
                    help='Verbosity level (0-10)')
parser.add_argument('--var-inc', type=float, default=1.0,
                    help='Increment applied during bump operations')
default_script = os.path.join(os.path.dirname(__file__), "..", "examples", "pipelined_heap_test.txt")
parser.add_argument('--script', type=str, default=default_script,
                    help='Path to the script describing heap operations')
parser.add_argument('--onchip-levels', type=int, default=3,
                    help='On-chip heap levels K (0 = all on-chip). The harness '
                         'default of 3 exercises the OLC on tiny heaps.')
args = parser.parse_args()

var_act_base_addr = 0x70000000

# Create test component
test_component = sst.Component("test", "satsolver.PipelinedHeapTest")
test_component.addParams({
    "verbose": str(args.verbose),
    "var_inc": str(args.var_inc),
    "clock": "1GHz",
    "script_path": os.path.abspath(args.script),
})

# Create heap subcomponent; it owns its memory interface
heap = test_component.setSubComponent("heap", "satsolver.PipelinedHeap")
heap.addParams({
    "verbose" : str(args.verbose),
    "var_act_base_addr" : hex(var_act_base_addr),
    # [acts | nodes] partition: 1 MiB region leaves ~64K node slots, far more
    # than any harness script needs, while keeping addresses inside the
    # memory controller's range.
    "heap_region_end" : hex(var_act_base_addr + 0x100000),
    "onchip_levels" : str(args.onchip_levels),
})

# Connect test component to heap
test_to_heap = sst.Link("test_to_heap")
test_to_heap.connect((test_component, "heap_port", "1ns"), (heap, "response", "1ns"))

# Configure the heap's own memory interface
memory = heap.setSubComponent("memory", "memHierarchy.standardInterface")

# Create memory controller for heap
memctrl = sst.Component("global_memory", "memHierarchy.MemController")
memctrl.addParams({
    "clock": "1GHz",
    "addr_range_start": "0x0",
    "addr_range_end": "0xFFFFFFFF",
})

# Create backend memory for heap
mem = memctrl.setSubComponent("backend", "memHierarchy.simpleMem")
mem.addParams({
    "access_time" : "1ns",
    "mem_size" : "4GiB",
    "max_requests_per_cycle" : "-1",
    "request_width" : "64",
})

# Connect heap to memory
heap_to_mem = sst.Link("heap_to_mem")
heap_to_mem.connect((memory, "lowlink", "1ns"), (memctrl, "highlink", "1ns"))


# Set statistics output
sst.setStatisticOutput("sst.statOutputConsole")
sst.enableStatisticsForComponentType("satsolver.PipelinedHeap", [
    "heap_insert_skips", "heap_stale_created", "heap_stale_pops",
    "heap_tail_trims", "heap_purge_pops", "heap_rebuilds",
    "heap_size_sample", "heap_stale_sample",
    "olc_node_reads", "olc_node_writes", "olc_boundary_crossings",
    "olc_insert_ctx_sample", "olc_sift_parked_sample",
    "olc_tail_refills", "olc_tail_stalls",
    "heap_rescales",
    "heap_cleans",
    "heap_clean_crossings",
    "heap_clean_search_misses",
    "heap_mint_drops",
], {"type": "sst.AccumulatorStatistic", "rate": "0ns"})

print("Running PipelinedHeap manual test (verbose={}, var_inc={})".format(
    args.verbose, args.var_inc))
