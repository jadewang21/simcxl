from m5.params import *
from m5.objects.PciDevice import *
from m5.objects.XBar import CXLMemBar


class CXLMemCtrl(PciDevice):
    type = 'CXLMemCtrl'
    cxx_header = "dev/x86/cxl_mem_ctrl.hh"
    cxx_class = 'gem5::CXLMemCtrl'

    cxl_rsp_port = ResponsePort(
        "This port sends responses to and receives requests from the Host"
    )
    mem_req_port = RequestPort(
        "This port sends requests to and receives responses from the back-end memory media"
    )

    rsp_size = Param.Unsigned(48, "The number of responses to buffer")
    req_size = Param.Unsigned(48, "The number of requests to buffer")
    
    proto_proc_lat = Param.Latency("15ns", "Latency of the CXL controller processing CXL.mem sub-protocol packets")
    cxl_mem_range = Param.AddrRange("2GB", "CXL expander memory range that can be identified as system memory")

    VendorID = 0x8086
    DeviceID = 0X7890
    Command = 0x0
    Status = 0x280
    Revision = 0x0
    ClassCode = 0x05
    SubClassCode = 0x00
    ProgIF = 0x00
    InterruptLine = 0x1f
    InterruptPin = 0x01

    # Primary
    BAR0 = PciMemBar(size='2GB')
    BAR1 = PciMemUpperBar()
    BAR2 = PciMemBar(size="2MiB")
    BAR3 = PciMemUpperBar()
    BAR4 = PciMemBar(size="512KiB")
    BAR5 = PciMemUpperBar()

    def connectMemory(self, cxl_mem_range, cxl_memory):
        self.cxl_mem_range = cxl_mem_range
        self.BAR0.size = cxl_memory.get_size_str()
        self.cxl_mem_bus = CXLMemBar()
        self.cxl_mem_bus.cpu_side_ports = self.mem_req_port
        for _, port in cxl_memory.get_mem_ports():
            self.cxl_mem_bus.mem_side_ports = port

    def configCXL(self, proc_lat, queue_size):
        self.proto_proc_lat = proc_lat
        self.rsp_size = queue_size
        self.req_size = queue_size

class CXLType1Accel(PciDevice):
    type = 'CXLType1Accel'
    cxx_header = "dev/x86/cxl_type1_accel.hh"
    cxx_class = 'gem5::CXLType1Accel'
    dcache_port = RequestPort("Data Port")
    icache_port = RequestPort("Intr Port")
    cacheline_size = Param.Int(64, "Device cache line size ")
    lsu_mode = Param.Int(1, "1: single-point; 2: sequential; 3: random; 4: write-then-read")
    lsu_num = Param.Int(1, "Number of cacheline requests per phase")
    load_store = Param.Int(1, "1 for load; 2 for store (ignored in mode 4)")

    VendorID = 0x8086
    DeviceID = 0X7890
    Command = 0x0
    Status = 0x280
    Revision = 0x0
    ClassCode = 0x05
    SubClassCode = 0x00
    ProgIF = 0x00
    InterruptLine = 0x1f
    InterruptPin = 0x01

    # Primary
    BAR0 = PciMemBar(size='4MiB')
    BAR1 = PciMemUpperBar()

    def connectCachedPorts(self, in_ports):
        self.dcache_port = in_ports

    def configCXL(self, lsu_mode, lsu_num, load_store):
        self.lsu_mode = lsu_mode
        self.lsu_num = lsu_num
        self.load_store = load_store

class CXLType2Accel(PciDevice):
    type = 'CXLType2Accel'
    cxx_header = "dev/x86/cxl_type2_accel.hh"
    cxx_class = 'gem5::CXLType2Accel'
    dcache_port = RequestPort("D2H data port (through Ruby HMC)")
    icache_port = RequestPort("Intr Port")
    dev_mem_port = RequestPort("D2D direct port bypassing CXL Controller (MICRO'24)")
    cacheline_size = Param.Int(64, "Device cache line size ")
    cxl_rsp_port = ResponsePort(
        "This port sends responses to and receives requests from the Host"
    )
    mem_req_port = RequestPort(
        "This port sends requests to and receives responses from the back-end memory media"
    )
    rsp_size = Param.Unsigned(48, "The number of responses to buffer")
    req_size = Param.Unsigned(48, "The number of requests to buffer")
    proto_proc_lat = Param.Latency("15ns", "Latency of the CXL controller processing CXL.mem sub-protocol packets")
    cxl_mem_range = Param.AddrRange("2GB", "CXL expander memory range that can be identified as system memory")
    dmc_size = Param.MemorySize("32kB", "DMC size per MICRO'24 (0 = disabled)")
    dmc_hit_latency = Param.Latency("5ns", "DMC hit latency (MICRO'24: similar to HMC hit)")
    lsu_mode = Param.Int(1, "1: single-point; 2: sequential; 3: random; 5/6: simplified AllReduce; 7/8: Ring AR CBC 1.0; 9: Baseline HBM-Centric; 10: Host-Centric CBC 2.0; 11: Mode 9 + L2 Injection Prefetch")
    lsu_num = Param.Int(1, "Number of cacheline requests per phase")
    load_store = Param.Int(1, "1 for load; 2 for store (ignored in modes 5-10)")
    allreduce_rounds = Param.Int(4, "Number of AllReduce rounds (modes 5/6)")
    num_npus = Param.Int(4, "Number of NPUs in Ring AllReduce (modes 7-10)")
    npu_id = Param.Int(0, "NPU ID in the ring (0-based)")
    compute_lat_per_line = Param.Latency("40ns", "Compute latency per cache line for reduction (400MHz FPGA, 2x FP16 adder)")
    prefetch_ownership = Param.Bool(False, "Enable prefetch-for-ownership: overlap GETX with RS Compute (Mode 10)")

    VendorID = 0x8086
    DeviceID = 0X7890
    Command = 0x0
    Status = 0x280
    Revision = 0x0
    ClassCode = 0x05
    SubClassCode = 0x00
    ProgIF = 0x00
    InterruptLine = 0x1f
    InterruptPin = 0x01

    # Primary
    BAR0 = PciMemBar(size='2GB')
    BAR1 = PciMemUpperBar()
    BAR2 = PciMemBar(size="2MiB")
    BAR3 = PciMemUpperBar()
    BAR4 = PciMemBar(size="512KiB")
    BAR5 = PciMemUpperBar()

    def connectCachedPorts(self, in_ports):
        self.dcache_port = in_ports

    def connectMemory(self, cxl_mem_range, cxl_memory):
        self.cxl_mem_range = cxl_mem_range
        self.BAR0.size = cxl_memory.get_size_str()
        self.cxl_mem_bus = CXLMemBar()
        self.cxl_mem_bus.cpu_side_ports = self.mem_req_port
        self.cxl_mem_bus.cpu_side_ports = self.dev_mem_port
        for _, port in cxl_memory.get_mem_ports():
            self.cxl_mem_bus.mem_side_ports = port

    def configCXL(self, proc_lat, queue_size, lsu_mode, lsu_num, load_store,
                  allreduce_rounds=4, num_npus=4, compute_lat_per_line="40ns",
                  npu_id=0, prefetch_ownership=False):
        self.proto_proc_lat = proc_lat
        self.rsp_size = queue_size
        self.req_size = queue_size
        self.lsu_mode = lsu_mode
        self.lsu_num = lsu_num
        self.load_store = load_store
        self.allreduce_rounds = allreduce_rounds
        self.num_npus = num_npus
        self.npu_id = npu_id
        self.compute_lat_per_line = compute_lat_per_line
        self.prefetch_ownership = prefetch_ownership
