#!/bin/bash
set -euo pipefail

SIMCXL_DIR="$(cd "$(dirname "$0")" && pwd)"
GEM5="${SIMCXL_DIR}/build/X86_CXL_MESI/gem5.opt"
CONFIG="${SIMCXL_DIR}/configs/example/gem5_library/x86-cxl-type1-motivation.py"
RESULTS="${SIMCXL_DIR}/results"

mkdir -p "${RESULTS}/rand_wide"
CSV="${RESULTS}/rand_wide_summary.csv"
echo "experiment,lsu_num,load_store,totalLoadLatency,per_request" > "${CSV}"

for num in 32 64 128 256 512; do
  for op in 1 2; do
    opname=$([[ $op -eq 1 ]] && echo "load" || echo "store")
    exp="rand_wide_${opname}_${num}"
    outdir="${RESULTS}/rand_wide/${exp}"
    mkdir -p "${outdir}"

    echo "=== ${exp} ==="
    "${GEM5}" --outdir="${outdir}" "${CONFIG}" \
        --lsu-mode=3 --lsu-num=${num} --load-store=${op} \
        > "${outdir}/console.log" 2>&1

    lat=$(grep "totalLoadLatency" "${outdir}/stats.txt" | awk '{print $2}' | tail -1)
    per=$(python3 -c "print(int(${lat}) // ${num})")
    echo "    total=${lat}  per_req=${per}"
    echo "${exp},${num},${op},${lat},${per}" >> "${CSV}"
  done
done

echo ""
echo "=== Done ==="
column -t -s',' "${CSV}"
