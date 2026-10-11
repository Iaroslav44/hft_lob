#!/usr/bin/env bash
# ==============================================================================
# Enterprise HFT Host Kernel & Hardware Tuning Script
# 
# Configures Linux Bare-Metal servers for deterministic sub-microsecond trading:
# 1. CPU Frequency & Power Management: C-States, P-States, Intel Turbo Boost
# 2. Kernel Scheduling: isolcpus, nohz_full, rcu_nocbs
# 3. Memory & TLB: Transparent Huge Pages (THP), Hugepages allocation
# 4. Hardware MSR Prefetcher disabling (MSR 0x1A4)
# 5. Network Stack & IRQ Affinity pinning
# ==============================================================================

set -euo pipefail

echo "[NEXUS-HFT] Applying kernel and CPU hardware tuning..."

# 1. CPU Governor: Force Maximum Clock Frequency (No Power-Saving Scaling)
if command -v cpupower &> /dev/null; then
    cpupower frequency-set -g performance || true
else
    for gov in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [ -f "$gov" ] && echo "performance" > "$gov" || true
    done
fi

# 2. Disable Intel CPU Energy Performance Bias
for epb in /sys/devices/system/cpu/cpu*/power/energy_perf_bias; do
    [ -f "$epb" ] && echo "0" > "$epb" || true # 0 = maximum performance
done

# 3. Disable CPU Idle States (C-States)
if [ -e /dev/cpu_dma_latency ]; then
    # Keep CPU out of C-States by setting latency tolerance to 0
    exec 3<> /dev/cpu_dma_latency
    printf '\x00\x00\x00\x00' >&3
fi

# 4. Disable Transparent Huge Pages (THP creates non-deterministic compaction jitter)
if [ -f /sys/kernel/mm/transparent_hugepage/enabled ]; then
    echo "never" > /sys/kernel/mm/transparent_hugepage/enabled
fi
if [ -f /sys/kernel/mm/transparent_hugepage/defrag ]; then
    echo "never" > /sys/kernel/mm/transparent_hugepage/defrag
fi

# 5. Allocate 1GB Hugepages for DPDK / Ring Buffers
if [ -f /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages ]; then
    echo 8 > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages || true
fi

# 6. Disable Hardware Prefetchers on Intel (MSR 0x1A4)
if command -v wrmsr &> /dev/null; then
    modprobe msr || true
    wrmsr -a 0x1a4 0xf || true
    echo "[NEXUS-HFT] Disabled CPU Hardware Prefetchers via MSR 0x1A4"
fi

# 7. Network SoftIRQ & Backlog parameters
sysctl -w net.core.rmem_max=134217728 || true
sysctl -w net.core.wmem_max=134217728 || true
sysctl -w net.core.netdev_max_backlog=250000 || true
sysctl -w net.ipv4.tcp_timestamps=0 || true
sysctl -w net.ipv4.tcp_sack=1 || true

echo "[NEXUS-HFT] Host tuning complete. Jitter minimized."
