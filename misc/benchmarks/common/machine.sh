#!/bin/sh
# Print the machine description that every benchmark README quotes.
# Works on macOS (sysctl) and Linux (/proc, lscpu). Run from anywhere.
set -u
echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "os: $(uname -srm)"
if command -v sw_vers >/dev/null 2>&1; then
  echo "macos: $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
fi
if command -v sysctl >/dev/null 2>&1 && sysctl -n machdep.cpu.brand_string >/dev/null 2>&1; then
  echo "cpu: $(sysctl -n machdep.cpu.brand_string)"
  echo "logical cpus: $(sysctl -n hw.logicalcpu)"
  p=$(sysctl -n hw.perflevel0.physicalcpu 2>/dev/null || echo ?)
  e=$(sysctl -n hw.perflevel1.physicalcpu 2>/dev/null || echo ?)
  echo "performance cores: $p  efficiency cores: $e  smt: none"
  echo "P-core L1d: $(sysctl -n hw.perflevel0.l1dcachesize) B  L2 (shared per cluster of $(sysctl -n hw.perflevel0.cpusperl2)): $(sysctl -n hw.perflevel0.l2cachesize) B"
  echo "E-core L1d: $(sysctl -n hw.perflevel1.l1dcachesize) B  L2 (shared per cluster of $(sysctl -n hw.perflevel1.cpusperl2)): $(sysctl -n hw.perflevel1.l2cachesize) B"
  echo "cache line: $(sysctl -n hw.cachelinesize) B  page: $(sysctl -n hw.pagesize) B  memory: $(( $(sysctl -n hw.memsize) / 1073741824 )) GiB"
  feats=$(sysctl -a 2>/dev/null | awk -F'[:. ]+' '/hw.optional.arm.FEAT_/ && $NF==1 {printf "%s ", $4}' | sed 's/FEAT_//g')
  echo "isa features: $feats"
  echo "power: $(pmset -g batt 2>/dev/null | head -1 | sed "s/Now drawing from //")"
elif [ -r /proc/cpuinfo ]; then
  echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
  command -v lscpu >/dev/null 2>&1 && lscpu | grep -E 'Thread|Core|Socket|NUMA|L1d|L2|L3|MHz' | sed 's/  */ /g'
fi
echo "load average at start: $(uptime | sed "s/.*load average[s]*: //")"
echo "frequency: not published by the vendor for this part; see estimated clock below"
if command -v cc >/dev/null 2>&1; then
  echo "compiler: $(cc --version | head -1)"
fi
