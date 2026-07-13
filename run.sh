#!/usr/bin/env bash
# ChaosEngine — one-shot build & run
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build"
BIN="$BUILD/bin"

TARGET="chaos_headless"
BUILD_TYPE="Debug"
REBUILD=0
RUN_AFTER=1
EXTRA_ARGS=()

usage() {
    cat <<EOF
Usage: ./run.sh [OPTIONS] [-- exe_args...]

Targets (default: --headless):
  --headless   chaos_headless  (server/CI mode, no GPU)
  --server     chaos_server    (game server with admin IPC)
  --editor     chaos_editor    (C++ editor)
  --client     chaos_client    (Vulkan rendering client)
  --gateway    chaos_gateway   (Lua gateway process)
  --router     chaos_router    (Lua router process)
  --dbproxy    chaos_dbproxy   (Lua DBProxy process)

Build options:
  --release    Release build (default: Debug)
  --rebuild    Delete build dir and reconfigure
  --no-run     Build only, do not run

Examples:
  ./run.sh                      build + run chaos_headless
  ./run.sh --server             build + run chaos_server
  ./run.sh --editor --release   release build + run chaos_editor
  ./run.sh --server --no-run    build only
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --release)  BUILD_TYPE=Release ;;
        --rebuild)  REBUILD=1 ;;
        --no-run)   RUN_AFTER=0 ;;
        --headless) TARGET=chaos_headless ;;
        --server)   TARGET=chaos_server ;;
        --editor)   TARGET=chaos_editor ;;
        --client)   TARGET=chaos_client ;;
        --gateway)  TARGET=chaos_gateway ;;
        --router)   TARGET=chaos_router ;;
        --dbproxy)  TARGET=chaos_dbproxy ;;
        --help|-h)  usage; exit 0 ;;
        --)         shift; EXTRA_ARGS=("$@"); break ;;
        *)          EXTRA_ARGS+=("$1") ;;
    esac
    shift
done

JOBS=$(nproc 2>/dev/null || sysctl -n hw.logicalcpu 2>/dev/null || echo 4)

echo "============================================================"
echo " ChaosEngine  |  target: $TARGET  |  $BUILD_TYPE"
echo "============================================================"

# ── Step 1: cmake configure ──
if [[ "$REBUILD" -eq 1 ]]; then
    echo "[1/3] Clean rebuild - removing $BUILD..."
    rm -rf "$BUILD"
fi

if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
    echo "[1/3] CMake configure..."
    cmake -S "$ROOT" -B "$BUILD" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCHAOS_BUILD_EDITOR=ON
else
    echo "[1/3] CMake already configured - skipping"
fi

# ── Step 2: build ──
echo "[2/3] Building $TARGET..."
cmake --build "$BUILD" --target "$TARGET" --config "$BUILD_TYPE" -j "$JOBS"
echo "[OK] Build succeeded"

EXE="$BIN/$TARGET"
if [[ ! -f "$EXE" ]]; then
    echo "[ERROR] Cannot find $TARGET under $BIN"
    exit 1
fi

if [[ "$RUN_AFTER" -eq 0 ]]; then
    echo "[INFO] --no-run: skipping execution"
    echo "[INFO] Binary: $EXE"
    exit 0
fi

# ── Step 3: run ──
echo "[3/3] Running $TARGET..."
echo "============================================================"
echo ""
"$EXE" "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}"
RC=$?
echo ""
echo "============================================================"
if [[ $RC -eq 0 ]]; then
    echo "[OK] $TARGET exited normally"
else
    echo "[WARN] $TARGET exited with code $RC"
fi
exit $RC
