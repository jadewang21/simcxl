#!/bin/bash
#
# Motivation experiments: measure CXL.cache latency for various access patterns.
#
# Each experiment boots the full system via KVM (~10s), switches to Timing CPU,
# triggers the hardware LSU, and records totalLoadLatency in stats.txt.
#
# Results are collected into results/motivation_summary.csv for analysis.
#
# Usage:
#   conda deactivate  # ensure system python
#   export PATH=$(echo $PATH | tr ':' '\n' | grep -v anaconda3 | tr '\n' ':' | sed 's/:$//')
#   chmod +x run_motivation.sh
#   sudo ./run_motivation.sh           # run all experiments
#   sudo ./run_motivation.sh --dry-run # preview commands only
#

set -euo pipefail

SIMCXL_DIR="$(cd "$(dirname "$0")" && pwd)"
GEM5_BIN="${SIMCXL_DIR}/build/X86_CXL_MESI/gem5.opt"
CONFIG="${SIMCXL_DIR}/configs/example/gem5_library/x86-cxl-type1-motivation.py"
RESULTS_DIR="${SIMCXL_DIR}/results"

DRY_RUN=0
if [[ "${1:-}" == "--dry-run" ]]; then
    DRY_RUN=1
fi

# --- Experiment matrix ---
# lsu_mode: 2=sequential, 3=random
# lsu_num:  number of cacheline (64B) requests
# load_store: 1=load(read), 2=store(write)
LSU_MODES=(2 3)
LSU_NUMS=(32 64 128 256 512)
LOAD_STORES=(1 2)

MODE_NAMES=('' '' 'seq' 'rand')
OP_NAMES=('' 'load' 'store')

mkdir -p "${RESULTS_DIR}"
CSV="${RESULTS_DIR}/motivation_summary.csv"
echo "experiment,lsu_mode,lsu_num,load_store,totalLoadLatency_ticks,latency_per_request_ticks" > "${CSV}"

TOTAL=$(( ${#LSU_MODES[@]} * ${#LSU_NUMS[@]} * ${#LOAD_STORES[@]} ))
COUNT=0

for mode in "${LSU_MODES[@]}"; do
    for num in "${LSU_NUMS[@]}"; do
        for op in "${LOAD_STORES[@]}"; do
            COUNT=$((COUNT + 1))
            EXP_NAME="${MODE_NAMES[$mode]}_${OP_NAMES[$op]}_${num}"
            OUTDIR="${RESULTS_DIR}/${EXP_NAME}"

            echo "=== [${COUNT}/${TOTAL}] ${EXP_NAME} ==="
            echo "    mode=${mode} num=${num} op=${op}"

            if [[ $DRY_RUN -eq 1 ]]; then
                echo "    [DRY-RUN] ${GEM5_BIN} --outdir=${OUTDIR} ${CONFIG} --lsu-mode=${mode} --lsu-num=${num} --load-store=${op}"
                continue
            fi

            mkdir -p "${OUTDIR}"

            "${GEM5_BIN}" \
                --outdir="${OUTDIR}" \
                "${CONFIG}" \
                --lsu-mode="${mode}" \
                --lsu-num="${num}" \
                --load-store="${op}" \
                > "${OUTDIR}/console.log" 2>&1 || {
                    echo "    [FAILED] see ${OUTDIR}/console.log"
                    echo "${EXP_NAME},${mode},${num},${op},FAILED,FAILED" >> "${CSV}"
                    continue
                }

            STATS_FILE="${OUTDIR}/stats.txt"
            if [[ -f "${STATS_FILE}" ]]; then
                LATENCY=$(grep "totalLoadLatency" "${STATS_FILE}" | awk '{print $2}' | tail -1)
                if [[ -n "${LATENCY}" && "${LATENCY}" != "0" ]]; then
                    PER_REQ=$(python3 -c "print(int(${LATENCY}) // ${num})")
                    echo "    totalLoadLatency = ${LATENCY} ticks, per_request = ${PER_REQ} ticks"
                    echo "${EXP_NAME},${mode},${num},${op},${LATENCY},${PER_REQ}" >> "${CSV}"
                else
                    echo "    [WARN] totalLoadLatency not found or zero"
                    echo "${EXP_NAME},${mode},${num},${op},0,0" >> "${CSV}"
                fi
            else
                echo "    [WARN] stats.txt not found"
                echo "${EXP_NAME},${mode},${num},${op},NOSTATS,NOSTATS" >> "${CSV}"
            fi
        done
    done
done

echo ""
echo "=== All experiments complete ==="
echo "Summary: ${CSV}"
if [[ $DRY_RUN -eq 0 ]]; then
    echo ""
    column -t -s',' "${CSV}"
fi
