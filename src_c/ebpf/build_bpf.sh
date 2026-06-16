#!/bin/bash
#
# ChaosEngine: Build BPF object files for Phase 4.3-4.4
#
# Compiles BPF C programs to .o files using clang.
# Requires: clang, libbpf-dev, linux-headers
#
# Usage: ./build_bpf.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BPF_DIR="$SCRIPT_DIR"
OUT_DIR="$BPF_DIR"

CLANG="${CLANG:-clang}"
CLANG_FLAGS="-O2 -g -target bpf -Wall -Wextra"

# Find kernel headers
if [ -d "/usr/include/x86_64-linux-gnu" ]; then
    KERNEL_INCLUDE="-I/usr/include/x86_64-linux-gnu"
else
    KERNEL_INCLUDE=""
fi

BPF_INCLUDE="-I/usr/include"

echo "=== ChaosEngine BPF Compiler ==="
echo "BPF source dir: $BPF_DIR"
echo "Output dir:     $OUT_DIR"
echo "Clang:          $CLANG"
echo ""

compile_bpf() {
    local src="$1"
    local out="$2"
    echo "  CC    $src -> $out"
    $CLANG $CLANG_FLAGS $BPF_INCLUDE $KERNEL_INCLUDE -c "$src" -o "$out"
    echo "  OK    $(file "$out" | cut -d: -f2-)"
}

# Phase 4.3: Stream Parser
echo "--- Phase 4.3: Stream Parser ---"
compile_bpf "$BPF_DIR/ce_stream_parser_kern.c" "$OUT_DIR/ce_stream_parser_kern.o"

# Phase 4.4: io_uring + BPF
echo ""
echo "--- Phase 4.4: io_uring + BPF ---"
compile_bpf "$BPF_DIR/ce_uring_bpf_kern.c" "$OUT_DIR/ce_uring_bpf_kern.o"

echo ""
echo "=== Build complete ==="
echo ""
echo "Generated files:"
ls -la "$OUT_DIR"/*.o 2>/dev/null || echo "  (no .o files found)"
