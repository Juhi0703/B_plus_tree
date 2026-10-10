#!/usr/bin/env bash
# sysinfo.sh - detect the system parameters the B+ tree node size is derived from.
#
#   scripts/sysinfo.sh [index_dir] [env_out]
#
# Prints the virtual-memory page size, the block size of the filesystem that will hold the
# index files, the logical/physical sector size of the disk under it, the CPU cache-line size,
# RAM and CPU. With env_out it also writes KEY=VALUE lines that run_all.sh sources; PAGE_SIZE
# is passed to the benchmark as --page-size, i.e. node size = OS page size.
# (The benchmark also calls sysconf(_SC_PAGESIZE) itself when --page-size is not given.)
set -u
DIR="${1:-.}"
OUT="${2:-}"
KEY_SIZE="${KEY_SIZE:-8}"   # only used for the preview below; the program measures the data
VAL_SIZE="${VAL_SIZE:-64}"

PAGE_SIZE=$(getconf PAGESIZE 2>/dev/null || getconf PAGE_SIZE 2>/dev/null || echo 4096)

if stat -f -c %S "$DIR" >/dev/null 2>&1; then                # GNU stat (Linux)
  FS_BLOCK=$(stat -f -c %S "$DIR"); FS_TYPE=$(stat -f -c %T "$DIR")
else                                                        # BSD stat (macOS)
  FS_BLOCK=$(stat -f %k "$DIR" 2>/dev/null || echo unknown); FS_TYPE=$(df -T "$DIR" 2>/dev/null | awk 'NR==2{print $2}')
fi

DEV=$(df -P "$DIR" 2>/dev/null | awk 'NR==2{print $1}')
LOG_SEC=unknown; PHY_SEC=unknown; ROTATIONAL=unknown
if command -v lsblk >/dev/null 2>&1 && [ -b "$DEV" ]; then
  read -r LOG_SEC PHY_SEC ROTATIONAL < <(lsblk -ndo LOG-SEC,PHY-SEC,ROTA "$DEV" 2>/dev/null | head -1)
fi

CACHE_LINE=$(getconf LEVEL1_DCACHE_LINESIZE 2>/dev/null || sysctl -n hw.cachelinesize 2>/dev/null || echo unknown)
[ "$CACHE_LINE" = "0" ] && CACHE_LINE=unknown
if [ -r /proc/meminfo ]; then RAM_BYTES=$(awk '/MemTotal/{print $2*1024}' /proc/meminfo)
else RAM_BYTES=$(sysctl -n hw.memsize 2>/dev/null || echo unknown); fi
CPU=$(grep -m1 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2- | sed 's/^ *//')
[ -z "$CPU" ] && CPU=$(sysctl -n machdep.cpu.brand_string 2>/dev/null || echo unknown)
CPUS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo unknown)
OS=$(uname -srm)

# Preview of what the program will compute (header 16 B, child pointer 4 B)
LEAF_CAP=$(( (PAGE_SIZE - 16) / (KEY_SIZE + VAL_SIZE) ))
INT_KEYS=$(( (PAGE_SIZE - 16 - 4) / (KEY_SIZE + 4) ))

cat <<EOF
OS                        : $OS
CPU                       : $CPU ($CPUS logical CPUs)
RAM                       : $RAM_BYTES bytes
memory page size          : $PAGE_SIZE bytes   <- B+ tree node size
filesystem block size     : $FS_BLOCK bytes ($FS_TYPE on $DEV)
disk logical/physical sec : $LOG_SEC / $PHY_SEC bytes (rotational: $ROTATIONAL)
L1 cache line             : $CACHE_LINE bytes
node capacities for key=$KEY_SIZE B, payload=$VAL_SIZE B:
  leaf     = floor(($PAGE_SIZE - 16) / ($KEY_SIZE + $VAL_SIZE)) = $LEAF_CAP records
  internal = floor(($PAGE_SIZE - 16 - 4) / ($KEY_SIZE + 4)) = $INT_KEYS keys -> fan-out $((INT_KEYS + 1))
EOF

if [ -n "$OUT" ]; then
  cat > "$OUT" <<EOF
PAGE_SIZE=$PAGE_SIZE
FS_BLOCK_SIZE=$FS_BLOCK
FS_TYPE="$FS_TYPE"
DISK_LOGICAL_SECTOR=$LOG_SEC
DISK_PHYSICAL_SECTOR=$PHY_SEC
DISK_ROTATIONAL=$ROTATIONAL
CACHE_LINE=$CACHE_LINE
RAM_BYTES=$RAM_BYTES
CPUS=$CPUS
CPU="$CPU"
OS="$OS"
EOF
fi
