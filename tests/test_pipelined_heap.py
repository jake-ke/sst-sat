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

print("Running PipelinedHeap manual test (verbose={}, var_inc={})".format(
    args.verbose, args.var_inc))
