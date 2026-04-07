"""
Parameterized CXL Type 1 accelerator config for motivation experiments.

Measures CXL.cache access latency for different access patterns (sequential,
random) and operations (load, store) at various request counts.

Usage:
    sudo ./build/X86_CXL_MESI/gem5.opt \
        --outdir=m5out/seq_load_64 \
        configs/example/gem5_library/x86-cxl-type1-motivation.py \
        --lsu-mode=2 --lsu-num=64 --load-store=1
"""

import argparse
import sys

from gem5.utils.requires import requires
from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.boards.x86_board_cxl_type1 import X86BoardCXLType1
from gem5.components.cachehierarchies.ruby.cxl_mesi_two_level_cache_hierarchy import (
    CXLMESITwoLevelCacheHierarchy,
)
from gem5.components.memory.single_channel import DIMM_DDR5_4400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.isas import ISA
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.resources.resource import KernelResource, DiskImageResource

parser = argparse.ArgumentParser()
parser.add_argument("--lsu-mode", type=int, default=2,
                    help="1: single-point, 2: sequential, 3: random")
parser.add_argument("--lsu-num", type=int, default=64,
                    help="Number of cacheline requests")
parser.add_argument("--load-store", type=int, default=1,
                    help="1: load (read), 2: store (write)")
args = parser.parse_args()

requires(
    isa_required=ISA.X86,
    coherence_protocol_required=CoherenceProtocol.CXL_MESI_TWO_LEVEL,
    kvm_required=True,
)

cache_hierarchy = CXLMESITwoLevelCacheHierarchy(
    l1d_size="128KiB",
    l1d_assoc=8,
    l1i_size="32KiB",
    l1i_assoc=8,
    l2_size="512KiB",
    l2_assoc=16,
    num_l2_banks=1,
)

memory = DIMM_DDR5_4400(size="3GiB")

processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=1,
)

for proc in processor.start:
    proc.core.usePerf = False

board = X86BoardCXLType1(
    clk_freq="2.4GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    lsu_mode=args.lsu_mode,
    lsu_num=args.lsu_num,
    load_store=args.load_store,
)

command = (
    "m5 exit;"
    + "echo 'Switched to Timing CPU';"
    + "m5 resetstats;"
    + "/home/test_code/LSU_test;"
    + "m5 exit;"
)

board.set_kernel_disk_workload(
    kernel=KernelResource(local_path='/home/wang/llm/SimCXL-img/vmlinux'),
    disk_image=DiskImageResource(local_path='/home/wang/llm/SimCXL-img/parsec.img'),
    readfile_contents=command,
)

simulator = Simulator(
    board=board,
    on_exit_event={
        ExitEvent.EXIT: (func() for func in [processor.switch])
    },
)

print(f"[Motivation] lsu_mode={args.lsu_mode}, lsu_num={args.lsu_num}, "
      f"load_store={args.load_store}")
simulator.run()
