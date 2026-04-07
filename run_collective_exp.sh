#!/bin/bash
#
# Collective Buffer Cache (CBC) validation experiments
#
# HMC config: 128KB, 4-way set-associative = 2048 cachelines of 64B
#
# Experiment groups:
#   1. Mode 4 (Write-then-Read): varying buffer size vs HMC capacity
#   2. Mode 2 (Cold Read): baseline for cross-NPU read without local cache
#   3. Mode 1 (Single-point repeated): perfect HMC hit baseline

GEM5=./build/X86_CXL_MESI/gem5.opt
CONFIG=configs/example/gem5_library/x86-cxl-type1-motivation.py
RESULT_DIR=results/collective

mkdir -p ${RESULT_DIR}

CSV=${RESULT_DIR}/collective_summary.csv
echo "experiment,lsu_mode,lsu_num,totalLoadLatency,writePhaseLatency,readPhaseLatency" > ${CSV}

LSU_NUMS="32 128 512 1024 2048 4096"

run_one() {
    local name=$1 mode=$2 num=$3 ls=$4
    local outdir=${RESULT_DIR}/${name}
    echo "========================================="
    echo "  Running: ${name} (mode=${mode}, num=${num}, ls=${ls})"
    echo "  $(date '+%H:%M:%S')"
    echo "========================================="
    ${GEM5} --outdir=${outdir} ${CONFIG} \
        --lsu-mode=${mode} --lsu-num=${num} --load-store=${ls} 2>&1 \
        | tail -3

    local total=$(grep -oP 'cxl_device\.totalLoadLatency\s+\K[0-9]+' ${outdir}/stats.txt 2>/dev/null | tail -1)
    local wphl=$(grep -oP 'cxl_device\.writePhaseLatency\s+\K[0-9]+' ${outdir}/stats.txt 2>/dev/null | tail -1)
    local rphl=$(grep -oP 'cxl_device\.readPhaseLatency\s+\K[0-9]+' ${outdir}/stats.txt 2>/dev/null | tail -1)

    [ -z "$total" ] && total="N/A"
    [ -z "$wphl" ] && wphl="0"
    [ -z "$rphl" ] && rphl="0"

    echo "${name},${mode},${num},${total},${wphl},${rphl}" >> ${CSV}
    echo "  → total=${total}  write=${wphl}  read=${rphl}"
    echo ""
}

echo "============================================================"
echo "  Group 1: Mode 4 — Write-then-Read (CBC simulation)"
echo "  Buffer sizes from 2KB to 256KB vs HMC 128KB"
echo "============================================================"
for N in ${LSU_NUMS}; do
    run_one "wtr_${N}" 4 ${N} 1
done

echo "============================================================"
echo "  Group 2: Mode 2 — Cold Sequential Read (no local cache)"
echo "  Simulates cross-NPU read without CBC"
echo "============================================================"
for N in ${LSU_NUMS}; do
    run_one "cold_read_${N}" 2 ${N} 1
done

echo "============================================================"
echo "  Group 3: Mode 1 — Single-point (perfect HMC hit baseline)"
echo "============================================================"
for N in 32 512 2048; do
    run_one "single_pt_${N}" 1 ${N} 1
done

echo ""
echo "========================================="
echo "  All experiments complete!"
echo "  Results: ${CSV}"
echo "========================================="
echo ""
column -t -s, ${CSV}
