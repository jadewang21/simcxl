#!/bin/bash
#
# Phase 1: AllReduce CBC vs no-CBC experiments on CXL Type 2 accelerator
#
# HMC config: 128KB L1d, 8-way = 2048 cachelines of 64B
# Device memory: 8GB DDR5 at 0x100000000
#
# Experiment matrix:
#   Mode 5 (no-CBC): all rounds read/write host memory via CXL.cache
#   Mode 6 (CBC):    round 0 reads host, populates device HBM; rounds 1+ read device HBM
#   Varied: lsu_num (buffer size in cachelines), allreduce_rounds
#
# Key metrics:
#   arRound0ReadLat  — first round read latency (cold, both modes same)
#   arSteadyReadLat  — rounds 1+ total read latency (CBC should be much lower)
#   arTotalWriteLat  — total write latency
#   totalLoadLatency — end-to-end

GEM5=./build/X86_CXL_MESI/gem5.opt
CONFIG=configs/example/gem5_library/x86-cxl-type2-allreduce.py
RESULT_DIR=results/allreduce

mkdir -p ${RESULT_DIR}

CSV=${RESULT_DIR}/allreduce_summary.csv
echo "experiment,mode,lsu_num,rounds,totalLat,r0ReadLat,steadyReadLat,totalWriteLat" > ${CSV}

ROUNDS=4

run_one() {
    local name=$1 mode=$2 num=$3 rounds=$4
    local outdir=${RESULT_DIR}/${name}
    echo "========================================="
    echo "  Running: ${name} (mode=${mode}, num=${num}, rounds=${rounds})"
    echo "  $(date '+%H:%M:%S')"
    echo "========================================="
    ${GEM5} --outdir=${outdir} ${CONFIG} \
        --lsu-mode=${mode} --lsu-num=${num} --allreduce-rounds=${rounds} 2>&1 \
        | tail -5

    local stats=${outdir}/stats.txt
    local total=$(grep -oP 'cxl_device\.totalLoadLatency\s+\K[0-9]+' ${stats} 2>/dev/null | tail -1)
    local r0read=$(grep -oP 'cxl_device\.arRound0ReadLat\s+\K[0-9]+' ${stats} 2>/dev/null | tail -1)
    local steady=$(grep -oP 'cxl_device\.arSteadyReadLat\s+\K[0-9]+' ${stats} 2>/dev/null | tail -1)
    local wlat=$(grep -oP 'cxl_device\.arTotalWriteLat\s+\K[0-9]+' ${stats} 2>/dev/null | tail -1)

    [ -z "$total" ]  && total="N/A"
    [ -z "$r0read" ] && r0read="0"
    [ -z "$steady" ] && steady="0"
    [ -z "$wlat" ]   && wlat="0"

    echo "${name},${mode},${num},${rounds},${total},${r0read},${steady},${wlat}" >> ${CSV}
    echo "  → total=${total} r0read=${r0read} steady=${steady} write=${wlat}"
    echo ""
}

LSU_NUMS="64 256 1024 2048 4096"

echo "============================================================"
echo "  Group 1: Mode 5 — AllReduce WITHOUT CBC"
echo "  All rounds access host memory via CXL.cache"
echo "============================================================"
for N in ${LSU_NUMS}; do
    run_one "nocbc_${N}_r${ROUNDS}" 5 ${N} ${ROUNDS}
done

echo "============================================================"
echo "  Group 2: Mode 6 — AllReduce WITH CBC"
echo "  Round 0 from host; rounds 1+ from device HBM"
echo "============================================================"
for N in ${LSU_NUMS}; do
    run_one "cbc_${N}_r${ROUNDS}" 6 ${N} ${ROUNDS}
done

echo "============================================================"
echo "  Group 3: Scaling rounds (fixed buffer = 512 cachelines)"
echo "============================================================"
for R in 2 4 8 16; do
    run_one "nocbc_512_r${R}" 5 512 ${R}
    run_one "cbc_512_r${R}" 6 512 ${R}
done

echo ""
echo "========================================="
echo "  All experiments complete!"
echo "  Results: ${CSV}"
echo "========================================="
echo ""
column -t -s, ${CSV}
