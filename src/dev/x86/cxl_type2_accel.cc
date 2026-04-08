#include <cstring>
#include <random>

#include "base/trace.hh"
#include "dev/x86/cxl_type2_accel.hh"
#include "debug/CXLMemCtrl.hh"
#include "debug/CXLType1Accel.hh"
#include "sim/sim_exit.hh"

namespace gem5
{

// Static members for multi-NPU coordination
std::vector<CXLType2Accel*> CXLType2Accel::s_all_npus;
int CXLType2Accel::s_barrier_count = 0;
int CXLType2Accel::s_finished_count = 0;

CXLType2Accel::CXLResponsePort::CXLResponsePort(const std::string& _name,
                                        CXLType2Accel& _ctrl,
                                        CXLRequestPort& _memReqPort,
                                        Cycles _protoProcLat, int _resp_limit,
                                        AddrRange _devMemRange)
    : ResponsePort(_name), ctrl(_ctrl),
    memReqPort(_memReqPort), protoProcLat(_protoProcLat),
    devMemRange(_devMemRange), outstandingResponses(0), 
    retryReq(false), respQueueLimit(_resp_limit),
    sendEvent([this]{ trySendTiming(); }, _name)
{
}

CXLType2Accel::CXLRequestPort::CXLRequestPort(const std::string& _name,
                                    CXLType2Accel& _ctrl,
                                    CXLResponsePort& _cxlRspPort,
                                    Cycles _protoProcLat, int _req_limit)
    : RequestPort(_name), ctrl(_ctrl),
    cxlRspPort(_cxlRspPort),
    protoProcLat(_protoProcLat), reqQueueLimit(_req_limit),
    sendEvent([this]{ trySendTiming(); }, _name)
{
}

CXLType2Accel::CXLType2Accel(const Params &p)
    : PciDevice(p),
    lsu_mode(p.lsu_mode),
    lsu_num(p.lsu_num),
    load_store(p.load_store),
    cur_num(0),
    recv_num(0),
    put_param_finished(0),
    LSU_finished(0),
    stage(3),
    remote_data(nullptr),
    cur_paddr(0),
    dcachePort(this),
    icachePort(this),
    devMemPort(this),
    devmem_pkt(nullptr),
    cacheLineSize(p.cacheline_size),
    dmc_sets(p.dmc_size / p.cacheline_size),
    dmcHitLatency(ticksToCycles(p.dmc_hit_latency)),
    cxlMemRange(p.cxl_mem_range),
    allreduce_rounds(p.allreduce_rounds),
    ar_round(0),
    ar_is_read_phase(true),
    ar_phase_start(0),
    ar_total_start(0),
    dev_mem_base(p.cxl_mem_range.start() + 0x100000),
    num_npus(p.num_npus),
    ar_phase_type(0),
    ar_subround(0),
    chunk_size(lsu_num / p.num_npus),
    ar_rs_read_total(0),
    ar_rs_write_total(0),
    ar_ag_read_total(0),
    ar_ag_write_total(0),
    computeLatPerLine(p.compute_lat_per_line),
    ar_rs_compute_total(0),
    computeDoneEvent([this]{ arComputeDone(); }, name()),
    prefetchOwnership(p.prefetch_ownership),
    ar_prefetch_active(false),
    ar_prefetch_done(false),
    ar_waiting_for_prefetch(false),
    pf_cur_num(0),
    pf_recv_num(0),
    pf_total(0),
    pf_blocked_pkt(nullptr),
    pf_start_tick(0),
    pfNextEvent([this]{ issuePrefetchNext(); }, name()),
    barrierReleaseEvent([this]{ arDoTransition(); }, name()),
    accelStatus(Uninitialized),
    cxlRspPort(p.name + ".cxl_rsp_port", *this, memReqPort,
            ticksToCycles(p.proto_proc_lat), p.rsp_size, p.cxl_mem_range),
    memReqPort(p.name + ".mem_req_port", *this, cxlRspPort,
            ticksToCycles(p.proto_proc_lat), p.req_size),
    preRspTick(0),
    stats(*this),
    runEvent(*this),
    runStage2(*this)
    {
        npu_id = p.npu_id;
        remote_data = new uint8_t[64];
        std::memset(ar_read_lat, 0, sizeof(ar_read_lat));
        std::memset(ar_write_lat, 0, sizeof(ar_write_lat));
        std::memset(dev_mem_bases, 0, sizeof(dev_mem_bases));
        for (int i = 0; i < MAX_DMC_SETS; i++) {
            dmc[i].valid = false;
            dmc[i].dirty = false;
            dmc[i].tag = 0;
        }
        if (dmc_sets > MAX_DMC_SETS)
            dmc_sets = MAX_DMC_SETS;

        s_all_npus.push_back(this);

        DPRINTF(CXLMemCtrl, "BAR0_addr:0x%lx, BAR0_size:0x%lx\n",
            p.BAR0->addr(), p.BAR0->size());
        DPRINTF(CXLType1Accel,
                "D2D path: DMC %d sets (%dKB), hit_lat=%d cycles, "
                "devMem range=[%#lx,%#lx)\n",
                dmc_sets, dmc_sets * p.cacheline_size / 1024,
                (int)dmcHitLatency,
                cxlMemRange.start(), cxlMemRange.end());
        if (lsu_mode == 5 || lsu_mode == 6) {
            DPRINTF(CXLType1Accel, "AllReduce mode=%d rounds=%d dev_mem_base=%#lx\n",
                    lsu_mode, allreduce_rounds, dev_mem_base);
        }
        if (lsu_mode >= 7 && lsu_mode <= 10) {
            DPRINTF(CXLType1Accel,
                    "RingAllReduce mode=%d npu_id=%d num_npus=%d lsu_num=%d "
                    "chunk_size=%d dev_mem_base=%#lx compute_lat=%llu\n",
                    lsu_mode, npu_id, num_npus, lsu_num, chunk_size,
                    dev_mem_base, computeLatPerLine);
        }
    }

CXLType2Accel::CXLStats::CXLStats(CXLType2Accel &_ctrl)
    : statistics::Group(&_ctrl),

      ADD_STAT(totalLoadLatency, statistics::units::Tick::get(),
               "total lsu load latency"),
      ADD_STAT(arRound0ReadLat, statistics::units::Tick::get(),
               "AllReduce round 0 read phase latency"),
      ADD_STAT(arSteadyReadLat, statistics::units::Tick::get(),
               "AllReduce rounds 1+ total read latency"),
      ADD_STAT(arTotalWriteLat, statistics::units::Tick::get(),
               "AllReduce total write phase latency"),
      ADD_STAT(arRsReadLat, statistics::units::Tick::get(),
               "Ring AllReduce Reduce-Scatter total read latency"),
      ADD_STAT(arRsWriteLat, statistics::units::Tick::get(),
               "Ring AllReduce Reduce-Scatter total write latency"),
      ADD_STAT(arAgReadLat, statistics::units::Tick::get(),
               "Ring AllReduce AllGather total read latency"),
      ADD_STAT(arAgWriteLat, statistics::units::Tick::get(),
               "Ring AllReduce AllGather total write latency"),
      ADD_STAT(arComputeReadLat, statistics::units::Tick::get(),
               "Ring AllReduce compute-read phase latency"),
      ADD_STAT(arRsComputeLat, statistics::units::Tick::get(),
               "Ring AllReduce RS compute delay total"),
      ADD_STAT(arPrefetchLat, statistics::units::Tick::get(),
               "Ring AllReduce ownership prefetch latency"),
      ADD_STAT(dmcHits, statistics::units::Count::get(),
               "DMC (device memory cache) hit count"),
      ADD_STAT(dmcMisses, statistics::units::Count::get(),
               "DMC (device memory cache) miss count"),
      ADD_STAT(dmcWritebacks, statistics::units::Count::get(),
               "DMC dirty line writeback count"),
      ADD_STAT(reqQueFullEvents, statistics::units::Count::get(),
               "Number of times the request queue has become full"),
      ADD_STAT(reqRetryCounts, statistics::units::Count::get(),
               "Number of times the request was sent for retry"),
      ADD_STAT(rspQueFullEvents, statistics::units::Count::get(),
               "Number of times the response queue has become full"),
      ADD_STAT(reqSendFaild, statistics::units::Count::get(),
               "Number of times the request send failed"),
      ADD_STAT(rspSendFaild, statistics::units::Count::get(),
               "Number of times the response send failed"),
      ADD_STAT(reqSendSucceed, statistics::units::Count::get(),
               "Number of times the request send succeeded"),
      ADD_STAT(rspSendSucceed, statistics::units::Count::get(),
               "Number of times the response send succeeded"),
      ADD_STAT(reqQueueLenDist, "Request queue length distribution (Count)"),
      ADD_STAT(rspQueueLenDist, "Response queue length distribution (Count)"),
      ADD_STAT(rspOutStandDist, "outstandingResponses distribution (Count)"),
      ADD_STAT(reqQueueLatDist, "Response queue latency distribution (Tick)"),
      ADD_STAT(rspQueueLatDist, "Response queue latency distribution (Tick)"),
      ADD_STAT(memToCXLCtrlRsp, "Distribution of the time intervals between "
               "consecutive mem responses from the memory media to the CXLCtrl (Cycle)")
{ 
    reqQueueLenDist
        .init(0, 49, 10)
        .flags(statistics::nozero);
    rspQueueLenDist
        .init(0, 49, 10)
        .flags(statistics::nozero);
    rspOutStandDist
        .init(0, 49, 10)
        .flags(statistics::nozero);
    reqQueueLatDist
        .init(12000, 41999, 1000)
        .flags(statistics::nozero);
    rspQueueLatDist
        .init(12000, 41999, 1000)
        .flags(statistics::nozero);
    memToCXLCtrlRsp
        .init(0, 299, 10)
        .flags(statistics::nozero);
}


void
CXLType2Accel::TimingDevicePort::TickEvent::schedule(PacketPtr _pkt, Tick t)
{
    pkt = _pkt;
    device->schedule(this, t);
}

void
CXLType2Accel::DcachePort::recvTimingSnoopReq(PacketPtr pkt)
{
    DPRINTF(CXLType1Accel, "%s received atomic snoop pkt for addr:%#x %s\n",
            __func__, pkt->getAddr(), pkt->cmdString());
}

bool
CXLType2Accel::DcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(CXLType1Accel, "Received load/store response %#x\n", pkt->getAddr());

    if (device->accelStatus == ExecutingLoop) {
        device->recvData(pkt);
    } else if (device->accelStatus == Returning) {
        // No need to process
    } else {
        panic("Got a memory response at a bad time");
    }

    // delete pkt->req;
    delete pkt;
    return true;
}

void
CXLType2Accel::recvData(PacketPtr pkt)
{
    if (ar_prefetch_active) {
        pfResponseReceived();
        return;
    }

    if (pkt->isRead()) {
        pkt->writeData(remote_data);
        DPRINTF(CXLType1Accel, "Received LOAD data for addr %#x, recv_num: %d\n", 
                pkt->getAddr(), recv_num);
    } else if (pkt->isWrite()) {
        DPRINTF(CXLType1Accel, "Confirmed STORE completion for addr %#x, recv_num: %d\n", 
                pkt->getAddr(), recv_num);
    } else {
        panic("Unexpected packet type: %s", pkt->cmdString());
    }

    stage = 3;
    recv_num++;
    if (recv_num >= paddr.num) {
        DPRINTF(CXLType1Accel, "All LSU responses have been received!\n");
        if (lsu_mode >= 5 && lsu_mode <= 10) {
            arPhaseComplete();
        } else {
            LSUFinish();
        }
    }
}

void
CXLType2Accel::LSUFinish()
{
    accelStatus = Returning;
    stats.totalLoadLatency = clockEdge() - first_issue_time;

    if (lsu_mode == 5 || lsu_mode == 6) {
        stats.arRound0ReadLat = ar_read_lat[0];
        Tick steady = 0;
        for (int i = 1; i < allreduce_rounds; i++)
            steady += ar_read_lat[i];
        stats.arSteadyReadLat = steady;

        Tick wtotal = 0;
        for (int i = 0; i < allreduce_rounds; i++)
            wtotal += ar_write_lat[i];
        stats.arTotalWriteLat = wtotal;

        DPRINTF(CXLType1Accel,
                "AllReduce done: rounds=%d round0_read=%llu "
                "steady_read=%llu total_write=%llu total=%llu\n",
                allreduce_rounds, ar_read_lat[0], steady, wtotal,
                clockEdge() - first_issue_time);
    }

    if (lsu_mode >= 7 && lsu_mode <= 10) {
        stats.arRsReadLat = ar_rs_read_total;
        stats.arRsWriteLat = ar_rs_write_total;
        stats.arAgReadLat = ar_ag_read_total;
        stats.arAgWriteLat = ar_ag_write_total;
        if (lsu_mode == 9 || lsu_mode == 10)
            stats.arRsComputeLat = ar_rs_compute_total;

        DPRINTF(CXLType1Accel,
                "[NPU%d] RingAllReduce done: mode=%d npus=%d chunk=%d "
                "RS_read=%llu RS_compute=%llu RS_write=%llu "
                "AG_read=%llu AG_write=%llu "
                "compute_read=%llu total=%llu\n",
                npu_id, lsu_mode, num_npus, chunk_size,
                ar_rs_read_total, ar_rs_compute_total, ar_rs_write_total,
                ar_ag_read_total, ar_ag_write_total,
                (Tick)stats.arComputeReadLat.value(),
                clockEdge() - first_issue_time);
    }

    LSU_finished = 1;

    if (lsu_mode >= 5 && lsu_mode <= 10) {
        if (multiNpuMode()) {
            s_finished_count++;
            DPRINTF(CXLType1Accel,
                    "[NPU%d] finished (%d/%zu)\n",
                    npu_id, s_finished_count, s_all_npus.size());
            if (s_finished_count >= (int)s_all_npus.size())
                exitSimLoop("m5_exit instruction encountered", 0,
                            curTick() + 1000);
        } else {
            exitSimLoop("m5_exit instruction encountered", 0,
                        curTick() + 1000);
        }
    }
}

void
CXLType2Accel::arPhaseComplete()
{
    Tick now = clockEdge();
    Tick phase_lat = now - ar_phase_start;

    if (lsu_mode >= 7 && lsu_mode <= 10) {
        const char *phase_names[] = {"RS", "AG", "Compute"};

        if (ar_is_read_phase) {
            if (ar_phase_type == 0)
                ar_rs_read_total += phase_lat;
            else if (ar_phase_type == 1)
                ar_ag_read_total += phase_lat;

            DPRINTF(CXLType1Accel,
                    "[NPU%d] Ring %s subround %d read done: %llu ticks\n",
                    npu_id, phase_names[ar_phase_type], ar_subround,
                    phase_lat);
        } else {
            if (ar_phase_type == 0)
                ar_rs_write_total += phase_lat;
            else if (ar_phase_type == 1)
                ar_ag_write_total += phase_lat;

            DPRINTF(CXLType1Accel,
                    "[NPU%d] Ring %s subround %d write done: %llu ticks\n",
                    npu_id, phase_names[ar_phase_type], ar_subround,
                    phase_lat);
        }

        if (multiNpuMode()) {
            arBarrierReached();
        } else {
            arDoTransition();
        }
        return;
    }

    if (ar_is_read_phase) {
        ar_read_lat[ar_round] = phase_lat;
        DPRINTF(CXLType1Accel,
                "AR round %d read phase done: %llu ticks\n",
                ar_round, phase_lat);
        arStartPhase(false);
    } else {
        ar_write_lat[ar_round] = phase_lat;
        DPRINTF(CXLType1Accel,
                "AR round %d write phase done: %llu ticks\n",
                ar_round, phase_lat);
        ar_round++;
        if (ar_round < allreduce_rounds) {
            arStartPhase(true);
        } else {
            LSUFinish();
        }
    }
}

void
CXLType2Accel::arStartPhase(bool is_read)
{
    ar_is_read_phase = is_read;
    ar_phase_start = clockEdge();
    cur_num = 0;
    recv_num = 0;

    if (lsu_mode >= 7 && lsu_mode <= 10) {
        paddr.num = (ar_phase_type == 2) ? lsu_num : chunk_size;
        const char *phase_names[] = {"RS", "AG", "Compute"};
        DPRINTF(CXLType1Accel,
                "Ring %s subround %d %s phase (%d requests)\n",
                phase_names[ar_phase_type], ar_subround,
                is_read ? "READ" : "WRITE", paddr.num);
    } else {
        paddr.num = lsu_num;
        DPRINTF(CXLType1Accel,
                "AR starting round %d %s phase (%d requests)\n",
                ar_round, is_read ? "READ" : "WRITE", lsu_num);
    }
    stage1();
}

bool
CXLType2Accel::IcachePort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(CXLType1Accel, "Received fetch response %#x\n", pkt->getAddr());
    return true;
}

void
CXLType2Accel::IcachePort::recvReqRetry()
{
    DPRINTF(CXLType1Accel, "Received retry req\n");
}

void
CXLType2Accel::DcachePort::DTickEvent::process()
{
    device->completeDataAccess(pkt);
}

// --- D2D direct path (MICRO'24 architecture) ---

bool
CXLType2Accel::isDeviceAddr(Addr addr) const
{
    return cxlMemRange.contains(addr);
}

bool
CXLType2Accel::DevMemPort::recvTimingResp(PacketPtr pkt)
{
    DPRINTF(CXLType1Accel, "D2D devMem response for addr %#x\n",
            pkt->getAddr());

    if (device->accelStatus == ExecutingLoop) {
        device->d2dResponseComplete();
    }

    delete pkt;
    return true;
}

void
CXLType2Accel::DevMemPort::recvReqRetry()
{
    assert(device->devmem_pkt != nullptr);
    PacketPtr tmp = device->devmem_pkt;
    if (sendTimingReq(tmp)) {
        DPRINTF(CXLType1Accel, "D2D retry succeeded for addr %#x, cur_num: %d\n",
                tmp->getAddr(), device->cur_num);
        device->devmem_pkt = nullptr;
        device->cur_num++;
        if (device->cur_num < device->paddr.num)
            device->stage1();
    }
}

void
CXLType2Accel::d2dResponseComplete()
{
    stage = 3;
    recv_num++;
    if (recv_num >= paddr.num) {
        DPRINTF(CXLType1Accel, "All D2D responses received!\n");
        if (lsu_mode >= 5 && lsu_mode <= 10) {
            arPhaseComplete();
        } else {
            LSUFinish();
        }
    }
}

void
CXLType2Accel::arComputeDone()
{
    DPRINTF(CXLType1Accel,
            "[NPU%d] RS subround %d compute done\n",
            npu_id, ar_subround);
    ar_compute_done = true;
    if (multiNpuMode()) {
        arBarrierReached();
    } else {
        arStartPhase(false);
    }
}

void
CXLType2Accel::startOwnershipPrefetch()
{
    ar_prefetch_active = true;
    ar_prefetch_done = false;
    pf_cur_num = 0;
    pf_recv_num = 0;
    pf_total = (ar_phase_type == 2) ? lsu_num : chunk_size;
    pf_start_tick = curTick();
    DPRINTF(CXLType1Accel,
            "[NPU%d] starting ownership prefetch for %d lines "
            "(phase_type=%d, subround=%d)\n",
            npu_id, pf_total, ar_phase_type, ar_subround);
    issuePrefetchNext();
}

void
CXLType2Accel::issuePrefetchNext()
{
    if (pf_cur_num >= pf_total)
        return;

    bool saved_read = ar_is_read_phase;
    ar_is_read_phase = false;
    Addr addr = getPhyAddr(pf_cur_num);
    ar_is_read_phase = saved_read;

    RequestPtr req = std::make_shared<Request>(addr, cacheLineSize, 0, 0);
    PacketPtr pkt = Packet::createRead(req);
    pkt->dataDynamic<uint8_t>(new uint8_t[cacheLineSize]);

    if (!dcachePort.sendTimingReq(pkt)) {
        DPRINTF(CXLType1Accel,
                "[NPU%d] prefetch blocked at idx %d, addr %#x\n",
                npu_id, pf_cur_num, addr);
        pf_blocked_pkt = pkt;
    } else {
        DPRINTF(CXLType1Accel,
                "[NPU%d] prefetch sent idx %d, addr %#x\n",
                npu_id, pf_cur_num, addr);
        pf_cur_num++;
        if (pf_cur_num < pf_total) {
            if (!pfNextEvent.scheduled())
                schedule(pfNextEvent, nextCycle());
        }
    }
}

void
CXLType2Accel::pfResponseReceived()
{
    pf_recv_num++;
    DPRINTF(CXLType1Accel,
            "[NPU%d] prefetch response %d/%d\n",
            npu_id, pf_recv_num, pf_total);
    if (pf_recv_num >= pf_total) {
        ar_prefetch_active = false;
        ar_prefetch_done = true;
        stats.arPrefetchLat = curTick() - pf_start_tick;
        DPRINTF(CXLType1Accel,
                "[NPU%d] all ownership prefetches done (%llu ticks)\n",
                npu_id, curTick() - pf_start_tick);
        if (ar_waiting_for_prefetch) {
            ar_waiting_for_prefetch = false;
            arStartPhase(false);
        }
    }
}

void
CXLType2Accel::arBarrierReached()
{
    s_barrier_count++;
    DPRINTF(CXLType1Accel,
            "[NPU%d] barrier reached (%d/%zu)\n",
            npu_id, s_barrier_count, s_all_npus.size());

    if (s_barrier_count >= (int)s_all_npus.size()) {
        s_barrier_count = 0;
        for (auto *npu : s_all_npus) {
            if (!npu->barrierReleaseEvent.scheduled())
                npu->schedule(npu->barrierReleaseEvent, npu->clockEdge());
        }
    }
}

void
CXLType2Accel::arDoTransition()
{
    if (ar_is_read_phase) {
        if (ar_phase_type == 2) {
            Tick phase_lat = clockEdge() - ar_phase_start;
            stats.arComputeReadLat = phase_lat;
            LSUFinish();
        } else if ((lsu_mode == 9 || lsu_mode == 10) &&
                   ar_phase_type == 0 && computeLatPerLine > 0 &&
                   !ar_compute_done) {
            Tick delay = (Tick)chunk_size * computeLatPerLine;
            ar_rs_compute_total += delay;
            DPRINTF(CXLType1Accel,
                    "[NPU%d] RS subround %d compute delay: %llu ticks\n",
                    npu_id, ar_subround, delay);
            schedule(computeDoneEvent, curTick() + delay);
            if (prefetchOwnership && lsu_mode == 10) {
                startOwnershipPrefetch();
            }
        } else {
            ar_compute_done = false;
            if (prefetchOwnership && lsu_mode == 10 &&
                ar_phase_type == 0 && !ar_prefetch_done) {
                ar_waiting_for_prefetch = true;
                DPRINTF(CXLType1Accel,
                        "[NPU%d] compute done, waiting for prefetch\n",
                        npu_id);
                return;
            }
            ar_prefetch_done = false;
            ar_waiting_for_prefetch = false;
            arStartPhase(false);
        }
    } else {
        ar_subround++;
        if (ar_phase_type == 0 && ar_subround >= num_npus - 1) {
            ar_phase_type = 1;
            ar_subround = 0;
            DPRINTF(CXLType1Accel, "[NPU%d] RS complete, switching to AG\n",
                    npu_id);
            arStartPhase(true);
        } else if (ar_phase_type == 1 && ar_subround >= num_npus - 1) {
            ar_phase_type = 2;
            ar_subround = 0;
            DPRINTF(CXLType1Accel,
                    "[NPU%d] AG complete, switching to ComputeRead\n",
                    npu_id);
            arStartPhase(true);
        } else {
            arStartPhase(true);
        }
    }
}

void
CXLType2Accel::sendDataD2D(PacketPtr pkt, bool read)
{
    Addr addr = pkt->getAddr();

    if (dmc_sets <= 0) {
        stats.dmcMisses++;
        if (!devMemPort.sendTimingReq(pkt)) {
            devmem_pkt = pkt;
        } else {
            devmem_pkt = nullptr;
            cur_num++;
            if (cur_num < paddr.num)
                stage1();
        }
        return;
    }

    int set_index = (addr / cacheLineSize) % dmc_sets;
    Addr tag = addr / cacheLineSize / dmc_sets;
    bool hit = dmc[set_index].valid && dmc[set_index].tag == tag;

    if (read) {
        if (hit) {
            stats.dmcHits++;
            DPRINTF(CXLType1Accel,
                    "D2D DMC READ HIT addr %#x set=%d, cur_num: %d\n",
                    addr, set_index, cur_num);
            delete pkt;
            auto *event = new EventFunctionWrapper(
                [this]() { d2dResponseComplete(); }, name(), true);
            schedule(event, clockEdge(dmcHitLatency));
            cur_num++;
            if (cur_num < paddr.num)
                stage1();
        } else {
            stats.dmcMisses++;
            if (dmc[set_index].valid && dmc[set_index].dirty)
                stats.dmcWritebacks++;
            dmc[set_index].valid = true;
            dmc[set_index].dirty = false;
            dmc[set_index].tag = tag;
            DPRINTF(CXLType1Accel,
                    "D2D DMC READ MISS addr %#x set=%d → HBM, cur_num: %d\n",
                    addr, set_index, cur_num);
            if (!devMemPort.sendTimingReq(pkt)) {
                devmem_pkt = pkt;
            } else {
                devmem_pkt = nullptr;
                cur_num++;
                if (cur_num < paddr.num)
                    stage1();
            }
        }
    } else {
        // Write-back DMC: writes complete at SRAM speed.
        // Full cache line writes don't need to fetch old data.
        if (hit) {
            stats.dmcHits++;
            DPRINTF(CXLType1Accel,
                    "D2D DMC WRITE HIT addr %#x set=%d, cur_num: %d\n",
                    addr, set_index, cur_num);
        } else {
            stats.dmcMisses++;
            if (dmc[set_index].valid && dmc[set_index].dirty)
                stats.dmcWritebacks++;
            dmc[set_index].valid = true;
            dmc[set_index].tag = tag;
            DPRINTF(CXLType1Accel,
                    "D2D DMC WRITE MISS (alloc) addr %#x set=%d, cur_num: %d\n",
                    addr, set_index, cur_num);
        }
        dmc[set_index].dirty = true;

        delete pkt;
        auto *event = new EventFunctionWrapper(
            [this]() { d2dResponseComplete(); }, name(), true);
        schedule(event, clockEdge(dmcHitLatency));
        cur_num++;
        if (cur_num < paddr.num)
            stage1();
    }
}

void
CXLType2Accel::DcachePort::recvReqRetry()
{
    if (device->ar_prefetch_active && device->pf_blocked_pkt) {
        PacketPtr tmp = device->pf_blocked_pkt;
        if (sendTimingReq(tmp)) {
            DPRINTF(CXLType1Accel,
                    "[NPU%d] prefetch retry succeeded idx %d\n",
                    device->npu_id, device->pf_cur_num);
            device->pf_blocked_pkt = nullptr;
            device->pf_cur_num++;
            if (device->pf_cur_num < device->pf_total) {
                if (!device->pfNextEvent.scheduled())
                    device->schedule(device->pfNextEvent, device->nextCycle());
            }
        }
        return;
    }
    // we shouldn't get a retry unless we have a packet that we're
    // waiting to transmit
    assert(device->dcache_pkt != NULL);
    assert(device->status == DcacheRetry);
    PacketPtr tmp = device->dcache_pkt;
    if (tmp->senderState) {
        // This is a packet from a split access.
        SplitFragmentSenderState * send_state =
            dynamic_cast<SplitFragmentSenderState *>(tmp->senderState);
        assert(send_state);
        PacketPtr big_pkt = send_state->bigPkt;

        SplitMainSenderState * main_send_state =
            dynamic_cast<SplitMainSenderState *>(big_pkt->senderState);
        assert(main_send_state);

        if (sendTimingReq(tmp)) {
            // If we were able to send without retrying, record that fact
            // and try sending the other fragment.
            send_state->clearFromParent();
            int other_index = main_send_state->getPendingFragment();
            if (other_index > 0) {
                tmp = main_send_state->fragments[other_index];
                device->dcache_pkt = tmp;
                if ((big_pkt->isRead() && device->handleReadPacket(tmp)) ||
                        (big_pkt->isWrite() && device->handleWritePacket())) {
                    main_send_state->fragments[other_index] = NULL;
                }
            } else {
                device->status = DcacheWaitResponse;
                // memory system takes ownership of packet
                device->dcache_pkt = NULL;
            }
        }
    } else if (sendTimingReq(tmp)) {
        DPRINTF(CXLType1Accel, "request LOAD for addr %#x, cur_num: %d\n", device->cur_paddr, device->cur_num);
        device->status = DcacheWaitResponse;
        // memory system takes ownership of packet
        device->dcache_pkt = NULL;
        device->cur_num++;
        if (device->cur_num < device->paddr.num)
            device->stage1();
        else 
            DPRINTF(CXLType1Accel, "All LSU requests have been issued!\n");
    } else {
        DPRINTF(CXLType1Accel, "Received retry req-3\n");
    }
}

Port &
CXLType2Accel::getPort(const std::string &if_name, PortID idx)
{
    if (if_name == "dma") {
        return dmaPort;
    } else if (if_name == "cxl_rsp_port") {
        return cxlRspPort;
    } else if (if_name == "mem_req_port") {
        return memReqPort;
    } else if (if_name == "dcache_port") {
        return dcachePort;
    } else if (if_name == "dev_mem_port") {
        return devMemPort;
    } else if (if_name == "icache_port") {
        return icachePort;
    }
    else {
        return PioDevice::getPort(if_name, idx);
        fatal("%s does not have any master port named %s\n", name(), if_name);
    }
}

void
CXLType2Accel::init()
{
    if (!cxlRspPort.isConnected() || !memReqPort.isConnected()
         || !pioPort.isConnected() || !dcachePort.isConnected())
        panic("CXL port of %s not connected to anything!", name());

    pioPort.sendRangeChange();
    cxlRspPort.sendRangeChange();
}

AddrRangeList
CXLType2Accel::getAddrRanges() const
{
    DPRINTF(CXLMemCtrl, "PIO base AddrRanges:\n");
    AddrRangeList ranges = PciDevice::getAddrRanges();
    for (const auto &r : ranges) {
        DPRINTF(CXLMemCtrl,
                "  range [%#lx - %#lx) size %#lx\n",
                r.start(), r.end(), r.size());
    }
    return ranges;
}

Addr
CXLType2Accel::getPhyAddr(int index)
{
    Addr phy_addr = 0;
    thread_local static std::mt19937 generator(std::random_device{}());
    std::uniform_int_distribution<size_t> distribution(0, 63);
    switch(lsu_mode) {
        case 1:
            // single-point access, return the next address
            phy_addr = paddr.phy + cacheLineSize;
            // phy_addr = 0xb6900000;
            break;
        case 2:
            // sequential access
            phy_addr = paddr.phy + index * cacheLineSize;
            break;
        case 3:
            // random access
            phy_addr = paddr.phy + distribution(generator) * cacheLineSize;
            break;
        case 5:
            // AllReduce no-CBC: read and write both target host memory,
            // writes offset by lsu_num to avoid warming read cache lines
            if (ar_is_read_phase)
                phy_addr = paddr.phy + (index % lsu_num) * cacheLineSize;
            else
                phy_addr = paddr.phy +
                           ((lsu_num + index) % (2 * lsu_num)) * cacheLineSize;
            break;
        case 6:
            // AllReduce with CBC: round 0 reads from host, writes to device;
            // rounds 1+ read from device (CBC hit), write to host
            if (ar_round == 0) {
                if (ar_is_read_phase)
                    phy_addr = paddr.phy + (index % lsu_num) * cacheLineSize;
                else
                    phy_addr = dev_mem_base + (index % lsu_num) * cacheLineSize;
            } else {
                if (ar_is_read_phase)
                    phy_addr = dev_mem_base + (index % lsu_num) * cacheLineSize;
                else
                    phy_addr = paddr.phy +
                               ((lsu_num + index) % (2 * lsu_num)) * cacheLineSize;
            }
            break;
        case 7: {
            // Realistic Ring AllReduce without CBC
            // Each phase uses separate host address regions to simulate
            // multi-NPU coherence invalidation (cold reads every round)
            int co = ar_subround * chunk_size;
            int num = (ar_phase_type == 2) ? lsu_num : chunk_size;
            int idx = index % num;

            if (ar_phase_type == 0) {
                if (ar_is_read_phase)
                    phy_addr = paddr.phy + (co + idx) * cacheLineSize;
                else
                    phy_addr = paddr.phy +
                               (lsu_num + co + idx) * cacheLineSize;
            } else if (ar_phase_type == 1) {
                if (ar_is_read_phase)
                    phy_addr = paddr.phy +
                               (2 * lsu_num + co + idx) * cacheLineSize;
                else
                    phy_addr = paddr.phy +
                               (3 * lsu_num + co + idx) * cacheLineSize;
            } else {
                phy_addr = paddr.phy +
                           (4 * lsu_num + idx) * cacheLineSize;
            }
            break;
        }
        case 8: {
            // Realistic Ring AllReduce WITH CBC
            // RS: read from host (receive), write to CBC (cache locally)
            // AG: read from CBC (forward cached data), write to host (send)
            // Compute: read from CBC (reuse AllReduce result)
            int co = ar_subround * chunk_size;
            int num = (ar_phase_type == 2) ? lsu_num : chunk_size;
            int idx = index % num;

            if (ar_phase_type == 0) {
                if (ar_is_read_phase)
                    phy_addr = paddr.phy + (co + idx) * cacheLineSize;
                else
                    phy_addr = dev_mem_base + (co + idx) * cacheLineSize;
            } else if (ar_phase_type == 1) {
                if (ar_is_read_phase)
                    phy_addr = dev_mem_base + (co + idx) * cacheLineSize;
                else
                    phy_addr = paddr.phy +
                               (lsu_num + co + idx) * cacheLineSize;
            } else {
                phy_addr = dev_mem_base + idx * cacheLineSize;
            }
            break;
        }
        case 9: {
            // Baseline HBM-Centric Ring AllReduce
            // Single-NPU: uses Host cold reads to simulate cross-NPU
            // Multi-NPU: AG Read targets predecessor's HBM (real cross-NPU!)
            int co = ar_subround * chunk_size;
            int num = (ar_phase_type == 2) ? lsu_num : chunk_size;
            int idx = index % num;

            if (ar_phase_type == 0) {          // Reduce-Scatter
                if (ar_is_read_phase) {
                    if (multiNpuMode() && ar_subround > 0) {
                        int src = (npu_id - 1 + num_npus) % num_npus;
                        phy_addr = dev_mem_bases[src] +
                                   (co + idx) * cacheLineSize;
                    } else {
                        phy_addr = paddr.phy + (co + idx) * cacheLineSize;
                    }
                } else {
                    phy_addr = dev_mem_base + (co + idx) * cacheLineSize;
                }
            } else if (ar_phase_type == 1) {   // AllGather
                if (ar_is_read_phase) {
                    if (multiNpuMode()) {
                        int src = (npu_id - 1 + num_npus) % num_npus;
                        phy_addr = dev_mem_bases[src] +
                                   (co + idx) * cacheLineSize;
                    } else {
                        phy_addr = paddr.phy +
                                   (2 * lsu_num + co + idx) * cacheLineSize;
                    }
                } else {
                    phy_addr = dev_mem_base +
                               (lsu_num + co + idx) * cacheLineSize;
                }
            } else {                           // Compute Read
                phy_addr = dev_mem_base + idx * cacheLineSize;
            }
            break;
        }
        case 10: {
            // Host-Centric CBC Ring AllReduce
            // All NPUs write RS results to per-NPU Host regions.
            // AG Read targets predecessor's Host region (HMC/LLC hit via
            // Ruby coherence snoop-forward, no HBM access needed).
            int co = ar_subround * chunk_size;
            int num = (ar_phase_type == 2) ? lsu_num : chunk_size;
            int idx = index % num;

            if (ar_phase_type == 0) {          // Reduce-Scatter
                if (ar_is_read_phase) {
                    if (multiNpuMode() && ar_subround > 0) {
                        int src = (npu_id - 1 + num_npus) % num_npus;
                        phy_addr = paddr.phy +
                                   ((1 + src) * lsu_num + co + idx) *
                                   cacheLineSize;
                    } else {
                        phy_addr = paddr.phy + (co + idx) * cacheLineSize;
                    }
                } else {
                    phy_addr = paddr.phy +
                               ((1 + npu_id) * lsu_num + co + idx) *
                               cacheLineSize;
                }
            } else if (ar_phase_type == 1) {   // AllGather
                if (ar_is_read_phase) {
                    if (multiNpuMode()) {
                        int src = (npu_id - 1 + num_npus) % num_npus;
                        phy_addr = paddr.phy +
                                   ((1 + src) * lsu_num + co + idx) *
                                   cacheLineSize;
                    } else {
                        phy_addr = paddr.phy +
                                   (lsu_num + co + idx) * cacheLineSize;
                    }
                } else {
                    phy_addr = paddr.phy +
                               ((1 + num_npus + npu_id) * lsu_num +
                                co + idx) * cacheLineSize;
                }
            } else {                           // Compute Read
                phy_addr = paddr.phy +
                           ((1 + npu_id) * lsu_num + idx) * cacheLineSize;
            }
            break;
        }
        default:
            phy_addr = paddr.phy;
    }
    return phy_addr;
}

Tick
CXLType2Accel::read(PacketPtr pkt)
{
    DPRINTF(CXLType1Accel, "read address : (%lx, %lx)\n", pkt->getAddr(),
            pkt->getSize());
    if(LSU_finished == 0){
        // haven't done
        pkt->setRaw(0);
    }else if(LSU_finished == 1){
        // done
        pkt->setRaw(12);
    }

    if (pkt->needsResponse()) {
        pkt->makeResponse();
    }
    return pioDelay;
}

Tick
CXLType2Accel::write(PacketPtr pkt)
{
    DPRINTF(CXLType1Accel, "write address : (%lx, %lx)\n", pkt->getAddr(),
            pkt->getSize());
    if(paddr.phy == 0) {
        pkt->writeData((uint8_t*)&paddr.phy);
        paddr.phy = 0xb4900000;
        DPRINTF(CXLType1Accel, "get paddr phy: %d\n", paddr.phy);
    } else if(paddr.num == 0) {
        pkt->writeData((uint8_t*)&paddr.num);
        paddr.num = lsu_num;
        DPRINTF(CXLType1Accel, "get paddr num: %d\n", paddr.num);
    } else if(put_param_finished == 0){
        pkt->writeData((uint8_t*)&put_param_finished);
        if(put_param_finished == 1) {
            accelStatus = Initialized;
            schedule(runEvent, clockEdge(Cycles(1)));   // perform LSU
        }
    }

    if (pkt->needsResponse()) {
        pkt->makeResponse();
    }
    return pioDelay;
}

void
CXLType2Accel::runLSU()
{
    assert(accelStatus == Initialized);
    accelStatus = ExecutingLoop;

    if (first_issue_time == (Tick)-1) {
        first_issue_time = clockEdge();
    }

    if (lsu_mode >= 7 && lsu_mode <= 10) {
        // Populate dev_mem_bases from all registered NPUs
        for (auto *npu : s_all_npus)
            dev_mem_bases[npu->npu_id] = npu->dev_mem_base;

        // NPU0 triggers all peer NPUs in multi-NPU mode
        if (multiNpuMode() && npu_id == 0) {
            s_barrier_count = 0;
            s_finished_count = 0;
            for (auto *npu : s_all_npus) {
                if (npu->npu_id != 0) {
                    npu->paddr.phy = paddr.phy;
                    npu->paddr.num = paddr.num;
                    for (auto *n2 : s_all_npus)
                        npu->dev_mem_bases[n2->npu_id] = n2->dev_mem_base;
                    npu->accelStatus = ExecutingLoop;
                    npu->first_issue_time = clockEdge();
                    npu->ar_phase_type = 0;
                    npu->ar_subround = 0;
                    npu->ar_rs_read_total = 0;
                    npu->ar_rs_write_total = 0;
                    npu->ar_ag_read_total = 0;
                    npu->ar_ag_write_total = 0;
                    npu->ar_rs_compute_total = 0;
                    npu->ar_compute_done = false;
                    npu->ar_total_start = clockEdge();
                    DPRINTF(CXLType1Accel,
                            "[NPU0] triggering NPU%d\n", npu->npu_id);
                    npu->arStartPhase(true);
                }
            }
        }

        ar_phase_type = 0;
        ar_subround = 0;
        ar_compute_done = false;
        ar_rs_read_total = 0;
        ar_rs_write_total = 0;
        ar_ag_read_total = 0;
        ar_ag_write_total = 0;
        ar_rs_compute_total = 0;
        ar_total_start = clockEdge();
        arStartPhase(true);
    } else if (lsu_mode == 5 || lsu_mode == 6) {
        ar_round = 0;
        ar_total_start = clockEdge();
        arStartPhase(true);
    } else {
        assert(stage == 3 && cur_num < paddr.num);
        stage1();
    }
}

void
CXLType2Accel::stage1()
{
    stage = 1;
    cur_paddr = this->getPhyAddr(cur_num);
    this->schedule(runStage2, this->nextCycle());
}

void
CXLType2Accel::stage2()
{
    stage = 2;
    bool is_read;
    if (lsu_mode >= 5 && lsu_mode <= 10)
        is_read = ar_is_read_phase;
    else
        is_read = (load_store == 1);

    uint8_t *temp = new uint8_t[64];
    if (is_read) {
        this->accessMemory(cur_paddr, 64, 1, temp);
    } else {
        std::memset(temp, 0, 64);
        this->accessMemory(cur_paddr, 64, 0, temp);
    }
}

void
CXLType2Accel::accessMemory(Addr paddr, int size, bool read, uint8_t *data)
{
    RequestPtr req = std::make_shared<Request>(paddr, size, 0, 0);
    this->sendData(req, data, read);
}

void
CXLType2Accel::sendData(const RequestPtr &req, uint8_t *data, bool read)
{
    PacketPtr pkt = buildPacket(req, read);
    pkt->dataDynamic<uint8_t>(data);

    if (req->getFlags().isSet(Request::NO_ACCESS)) {
        assert(!dcache_pkt);
        pkt->makeResponse();
        return;
    }

    Addr addr = req->getPaddr();
    if (isDeviceAddr(addr) && devMemPort.isConnected()) {
        sendDataD2D(pkt, read);
    } else if (read) {
        handleReadPacket(pkt);
    } else {
        dcache_pkt = pkt;
        handleWritePacket();
    }
}

PacketPtr
CXLType2Accel::buildPacket(const RequestPtr &req, bool read)
{
    return read ? Packet::createRead(req) : Packet::createWrite(req);
}

bool
CXLType2Accel::handleReadPacket(PacketPtr pkt)
{
    if (!dcachePort.sendTimingReq(pkt)) {
        DPRINTF(CXLType1Accel, "Failed to send LOAD request, curr_num: %d\n", cur_num);
        status = DcacheRetry;
        dcache_pkt = pkt;
    } else {
        DPRINTF(CXLType1Accel, "request LOAD for addr %#x, cur_num: %d\n", cur_paddr, cur_num);
        cur_num++;
        if (cur_num < paddr.num)
            stage1();
        else 
            DPRINTF(CXLType1Accel, "All LSU requests have been issued!\n");

        status = DcacheWaitResponse;
        // memory system takes ownership of packet
        dcache_pkt = NULL;
    }
    return dcache_pkt == NULL;
}

bool
CXLType2Accel::handleWritePacket()
{
    if (!dcachePort.sendTimingReq(dcache_pkt)) {
        DPRINTF(CXLType1Accel, "Failed to send STORE request, curr_num: %d\n", cur_num);
        status = DcacheRetry;
    } else {
        DPRINTF(CXLType1Accel, "request STORE for addr %#x, cur_num: %d\n", cur_paddr, cur_num);
        cur_num++;
        if (cur_num < paddr.num)
            stage1();
        else 
            DPRINTF(CXLType1Accel, "All LSU requests have been issued!\n");

        status = DcacheWaitResponse;
        // memory system takes ownership of packet
        dcache_pkt = NULL;
    }
    return dcache_pkt == NULL;
}

void
CXLType2Accel::completeDataAccess(PacketPtr pkt)
{
    DPRINTF(CXLType1Accel, "completeDataAccess: %s\n", pkt->cmdString());
}


bool
CXLType2Accel::CXLResponsePort::respQueueFull() const
{
    if (outstandingResponses == respQueueLimit) {
        ctrl.stats.rspQueFullEvents++;
        return true;
    } else {
        return false;
    }
}

bool
CXLType2Accel::CXLRequestPort::reqQueueFull() const
{
    if (transmitList.size() == reqQueueLimit) {
        ctrl.stats.reqQueFullEvents++;
        return true;
    } else {
        return false;
    }
}

bool
CXLType2Accel::CXLRequestPort::recvTimingResp(PacketPtr pkt)
{
    // all checks are done when the request is accepted on the response
    // side, so we are guaranteed to have space for the response
    DPRINTF(CXLMemCtrl, "recvTimingResp: %s addr 0x%x\n",
            pkt->cmdString(), pkt->getAddr());

    DPRINTF(CXLMemCtrl, "Request queue size: %d\n", transmitList.size());

    if (ctrl.preRspTick == -1) {
        ctrl.preRspTick = ctrl.clockEdge();
    } else {
        ctrl.stats.memToCXLCtrlRsp.sample(
            ctrl.ticksToCycles(ctrl.clockEdge() - ctrl.preRspTick));
        ctrl.preRspTick = ctrl.clockEdge();
    }

    // technically the packet only reaches us after the header delay,
    // and typically we also need to deserialise any payload
    Tick receive_delay = pkt->headerDelay + pkt->payloadDelay;
    pkt->headerDelay = pkt->payloadDelay = 0;

    cxlRspPort.schedTimingResp(pkt, ctrl.clockEdge(protoProcLat) +
                              receive_delay);

    return true;
}

bool
CXLType2Accel::CXLResponsePort::recvTimingReq(PacketPtr pkt)
{
    DPRINTF(CXLMemCtrl, "recvTimingReq: %s addr 0x%x\n",
            pkt->cmdString(), pkt->getAddr());

    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");

    if (retryReq)
        return false;

    DPRINTF(CXLMemCtrl, "Response queue size: %d outresp: %d\n",
            transmitList.size(), outstandingResponses);

    // if the request queue is full then there is no hope
    if (memReqPort.reqQueueFull()) {
        DPRINTF(CXLMemCtrl, "Request queue full\n");
        retryReq = true;
    } else {
        // look at the response queue if we expect to see a response
        bool expects_response = pkt->needsResponse();
        if (expects_response) {
            if (respQueueFull()) {
                DPRINTF(CXLMemCtrl, "Response queue full\n");
                retryReq = true;
            } else {
                // ok to send the request with space for the response
                DPRINTF(CXLMemCtrl, "Reserving space for response\n");
                assert(outstandingResponses != respQueueLimit);
                ++outstandingResponses;

                // no need to set retryReq to false as this is already the
                // case
                ctrl.stats.rspOutStandDist.sample(outstandingResponses);
            }
        }

        if (!retryReq) {
            Tick receive_delay = pkt->headerDelay + pkt->payloadDelay;
            pkt->headerDelay = pkt->payloadDelay = 0;

            memReqPort.schedTimingReq(pkt, ctrl.clockEdge(protoProcLat) +
                                      receive_delay);
        }
    }

    // remember that we are now stalling a packet and that we have to
    // tell the sending requestor to retry once space becomes available,
    // we make no distinction whether the stalling is due to the
    // request queue or response queue being full
    return !retryReq;
}

void
CXLType2Accel::CXLResponsePort::retryStalledReq()
{
    if (retryReq) {
        DPRINTF(CXLMemCtrl, "Request waiting for retry, now retrying\n");
        retryReq = false;
        sendRetryReq();
        ctrl.stats.reqRetryCounts++;
    }
}

void
CXLType2Accel::CXLRequestPort::schedTimingReq(PacketPtr pkt, Tick when)
{
    // If we're about to put this packet at the head of the queue, we
    // need to schedule an event to do the transmit.  Otherwise there
    // should already be an event scheduled for sending the head
    // packet.
    if (transmitList.empty()) {
        ctrl.schedule(sendEvent, when);
    }

    assert(transmitList.size() != reqQueueLimit);

    transmitList.emplace_back(pkt, when);

    ctrl.stats.reqQueueLenDist.sample(transmitList.size());
}

void
CXLType2Accel::CXLResponsePort::schedTimingResp(PacketPtr pkt, Tick when)
{
    if (transmitList.empty()) {
        ctrl.schedule(sendEvent, when);
    }

    transmitList.emplace_back(pkt, when);

    ctrl.stats.rspQueueLenDist.sample(transmitList.size());
}

void
CXLType2Accel::CXLRequestPort::trySendTiming()
{
    assert(!transmitList.empty());

    DeferredPacket req = transmitList.front();

    assert(req.tick <= curTick());

    PacketPtr pkt = req.pkt;

    DPRINTF(CXLMemCtrl, "trySend request addr 0x%x, queue size %d\n",
            pkt->getAddr(), transmitList.size());

    if (sendTimingReq(pkt)) {
        // send successful
        ctrl.stats.reqSendSucceed++;
        ctrl.stats.reqQueueLatDist.sample(curTick() - req.entryTime);

        transmitList.pop_front();

        ctrl.stats.reqQueueLenDist.sample(transmitList.size());
        DPRINTF(CXLMemCtrl, "trySend request successful\n");

        // If there are more packets to send, schedule event to try again.
        if (!transmitList.empty()) {
            DeferredPacket next_req = transmitList.front();
            DPRINTF(CXLMemCtrl, "Scheduling next send\n");
            ctrl.schedule(sendEvent, std::max(next_req.tick,
                                                ctrl.clockEdge()));
        }

        // if we have stalled a request due to a full request queue,
        // then send a retry at this point, also note that if the
        // request we stalled was waiting for the response queue
        // rather than the request queue we might stall it again
        cxlRspPort.retryStalledReq();
    } else {
        ctrl.stats.reqSendFaild++;
    }

    // if the send failed, then we try again once we receive a retry,
    // and therefore there is no need to take any action
}

void
CXLType2Accel::CXLResponsePort::trySendTiming()
{
    assert(!transmitList.empty());

    DeferredPacket resp = transmitList.front();

    assert(resp.tick <= curTick());

    PacketPtr pkt = resp.pkt;

    DPRINTF(CXLMemCtrl, "trySend response addr 0x%x, outstanding %d\n",
            pkt->getAddr(), outstandingResponses);

    if (sendTimingResp(pkt)) {
        // send successful
        ctrl.stats.rspSendSucceed++;
        ctrl.stats.rspQueueLatDist.sample(curTick() - resp.entryTime);

        transmitList.pop_front();

        ctrl.stats.rspQueueLenDist.sample(transmitList.size());
        DPRINTF(CXLMemCtrl, "trySend response successful\n");

        assert(outstandingResponses != 0);
        --outstandingResponses;

        ctrl.stats.rspOutStandDist.sample(outstandingResponses);

        // If there are more packets to send, schedule event to try again.
        if (!transmitList.empty()) {
            DeferredPacket next_resp = transmitList.front();
            DPRINTF(CXLMemCtrl, "Scheduling next send\n");
            ctrl.schedule(sendEvent, std::max(next_resp.tick,
                                                ctrl.clockEdge()));
        }

        // if there is space in the request queue and we were stalling
        // a request, it will definitely be possible to accept it now
        // since there is guaranteed space in the response queue
        if (!memReqPort.reqQueueFull() && retryReq) {
            DPRINTF(CXLMemCtrl, "Request waiting for retry, now retrying\n");
            retryReq = false;
            sendRetryReq();
            ctrl.stats.reqRetryCounts++;
        }
    } else {
        ctrl.stats.rspSendFaild++;
    }

    // if the send failed, then we try again once we receive a retry,
    // and therefore there is no need to take any action
}

void
CXLType2Accel::CXLRequestPort::recvReqRetry()
{
    trySendTiming();
}

void
CXLType2Accel::CXLResponsePort::recvRespRetry()
{
    trySendTiming();
}

Tick
CXLType2Accel::CXLResponsePort::recvAtomic(PacketPtr pkt)
{
    DPRINTF(CXLMemCtrl, "CXLMemCtrl recvAtomic: %s AddrRange: %s\n",
            pkt->cmdString(), pkt->getAddrRange().to_string());
    panic_if(pkt->cacheResponding(), "Should not see packets where cache "
             "is responding");
    
    Cycles delay = processCXLMem(pkt);

    Tick access_delay = memReqPort.sendAtomic(pkt);

    DPRINTF(CXLMemCtrl, "access_delay=%ld, proto_proc_lat=%ld, total=%ld\n",
            access_delay, delay, delay * ctrl.clockPeriod() + access_delay);
    return delay * ctrl.clockPeriod() + access_delay;
}

Tick
CXLType2Accel::CXLResponsePort::recvAtomicBackdoor(
    PacketPtr pkt, MemBackdoorPtr &backdoor)
{
    Cycles delay = processCXLMem(pkt);

    return delay * ctrl.clockPeriod() + memReqPort.sendAtomicBackdoor(
        pkt, backdoor);
}

Cycles
CXLType2Accel::CXLResponsePort::processCXLMem(PacketPtr pkt) {
    if (pkt->cxl_cmd == MemCmd::M2SReq) {
        assert(pkt->isRead());
    } else if (pkt->cxl_cmd == MemCmd::M2SRwD) {
        assert(pkt->isWrite());
    }
    return protoProcLat + protoProcLat;
}

AddrRangeList
CXLType2Accel::CXLResponsePort::getAddrRanges() const
{
    AddrRangeList ranges;
    ranges.push_back(devMemRange);
    DPRINTF(CXLMemCtrl, "CXLResponsePort base AddrRanges:\n");
    for (const auto &r : ranges) {
        DPRINTF(CXLMemCtrl,
                "  range [%#lx - %#lx) size %#lx\n",
                r.start(), r.end(), r.size());
    }

    DPRINTF(CXLMemCtrl,
            "CXLResponsePort adds devMemRange [%#lx - %#lx) size %#lx\n",
            devMemRange.start(), devMemRange.end(), devMemRange.size());
    return ranges;
}

} // namespace gem5