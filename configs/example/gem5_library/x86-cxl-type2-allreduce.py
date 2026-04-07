"""
CXL Type 2 AllReduce experiment config (multi-NPU support).

  Mode 9: Baseline HBM-Centric (RS write→HBM, AG cross-NPU read from peer HBM)
  Mode 10: Host-Centric CBC 2.0 (RS write→Host, AG read via Ruby coherence hit)

  With --num-npus=N (N>1), N real CXLType2Accel instances are created,
  each with its own L1/HMC, HBM, and Ruby Directory. Cross-NPU reads
  naturally traverse the CXL coherence protocol.

Usage:
    sudo ./build/X86_CXL_MESI/gem5.opt \\
        --outdir=results/multi_npu_m9 \\
        configs/example/gem5_library/x86-cxl-type2-allreduce.py \\
        --lsu-mode=9 --lsu-num=2048 --num-npus=2 --hbm-per-npu=256MiB
"""

import argparse

from gem5.utils.requires import requires
from gem5.coherence_protocol import CoherenceProtocol
from gem5.components.boards.x86_board_cxl_type2 import X86BoardCXLType2
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
parser.add_argument("--lsu-mode", type=int, default=9,
                    help="5/6: simplified; 7/8: CBC 1.0; 9: Baseline HBM-Centric; 10: Host-Centric CBC 2.0")
parser.add_argument("--lsu-num", type=int, default=2048,
                    help="Total cachelines in AllReduce buffer")
parser.add_argument("--allreduce-rounds", type=int, default=4,
                    help="Number of AllReduce rounds (modes 5/6 only)")
parser.add_argument("--num-npus", type=int, default=4,
                    help="Number of NPUs in ring (modes 7-10)")
parser.add_argument("--compute-lat", type=str, default="40ns",
                    help="Compute latency per cache line for reduction (modes 9/10)")
parser.add_argument("--hbm-per-npu", type=str, default="256MiB",
                    help="HBM size per NPU (used when num-npus > 1)")
parser.add_argument("--prefetch-ownership", action="store_true", default=False,
                    help="Enable prefetch-for-ownership: overlap GETX with RS Compute (Mode 10)")
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

if args.num_npus > 1:
    cxl_dram = [DIMM_DDR5_4400(size=args.hbm_per_npu)
                for _ in range(args.num_npus)]
else:
    cxl_dram = DIMM_DDR5_4400(size="8GB")

processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=1,
)

for proc in processor.start:
    proc.core.usePerf = False

board = X86BoardCXLType2(
    clk_freq="2.4GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
    cxl_memory=cxl_dram,
    lsu_mode=args.lsu_mode,
    lsu_num=args.lsu_num,
    load_store=1,
    allreduce_rounds=args.allreduce_rounds,
    num_npus=args.num_npus,
    compute_lat_per_line=args.compute_lat,
    prefetch_ownership=args.prefetch_ownership,
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

print(f"[AllReduce] mode={args.lsu_mode}, lsu_num={args.lsu_num}, "
      f"rounds={args.allreduce_rounds}, num_npus={args.num_npus}, "
      f"compute_lat={args.compute_lat}, "
      f"hbm_per_npu={args.hbm_per_npu if args.num_npus > 1 else '8GB'}, "
      f"prefetch_ownership={args.prefetch_ownership}")
simulator.run()
