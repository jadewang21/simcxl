#!/bin/bash
GEM5=/home/wang/llm/SimCXL/build/X86_CXL_MESI/gem5.opt
CONFIG=/home/wang/llm/SimCXL/configs/example/gem5_library/x86-cxl-type2-allreduce.py
RESDIR=/home/wang/llm/SimCXL/results

run_exp() {
    local name=$1 mode=$2 lsu_num=$3 npus=$4 clat=$5
    local outdir="${RESDIR}/${name}"
    if [ -f "${outdir}/stats.txt" ]; then
        echo "[SKIP] ${name} already done"
        return
    fi
    mkdir -p "${outdir}"
    echo "[RUN] ${name}: mode=${mode} lsu_num=${lsu_num} npus=${npus} compute_lat=${clat}"
    sudo -S -E env "PATH=$PATH" $GEM5 --outdir="${outdir}" $CONFIG \
        --lsu-mode=${mode} --lsu-num=${lsu_num} --num-npus=${npus} --compute-lat=${clat} \
        < /dev/null > "${outdir}/console.log" 2>&1
    echo "[DONE] ${name}: exit=$?"
}

echo wang | sudo -S true

# E1: Buffer size sweep (Mode 9 vs 10, 4 NPUs, compute=40ns)
for lsu in 128 256 1024 2048; do
    run_exp "e1_m9_${lsu}" 9 ${lsu} 4 40ns
    run_exp "e1_m10_${lsu}" 10 ${lsu} 4 40ns
done

# E2: NPU count sweep (128KB = 2048 lines, compute=40ns)
for npus in 2 8; do
    run_exp "e2_m9_npus${npus}" 9 2048 ${npus} 40ns
    run_exp "e2_m10_npus${npus}" 10 2048 ${npus} 40ns
done

# E3: Compute/communication ratio (128KB, 4 NPUs)
for clat in 0ns 10ns 100ns; do
    run_exp "e3_m9_clat${clat}" 9 2048 4 ${clat}
    run_exp "e3_m10_clat${clat}" 10 2048 4 ${clat}
done

echo "=== ALL EXPERIMENTS COMPLETE ==="
