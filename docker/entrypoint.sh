#!/bin/bash
# MOSS Docker entrypoint — build kernel and run in QEMU
#
# Usage:
#   entrypoint.sh run [--arch arm64|x86_64|riscv] [qemu-args...]
#   entrypoint.sh test [--arch arm64|x86_64|riscv]
#   entrypoint.sh build [--arch arm64|x86_64|riscv]
#   entrypoint.sh shell
set -euo pipefail

ARCH="${MOSS_ARCH:-arm64}"
BUILD_TYPE="${MOSS_BUILD_TYPE:-debug}"

# Parse --arch flag from arguments
parse_args() {
    local args=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --arch)
                ARCH="$2"
                shift 2
                ;;
            --arch=*)
                ARCH="${1#*=}"
                shift
                ;;
            *)
                args+=("$1")
                shift
                ;;
        esac
    done
    REMAINING_ARGS=("${args[@]+"${args[@]}"}")
}

PRESET="${ARCH}-${BUILD_TYPE}"

do_build() {
    echo "=== Building MOSS kernel (preset: ${PRESET}) ==="
    uv run build.py --preset "${PRESET}" --verbose
}

do_run() {
    do_build
    echo ""
    echo "=== Running MOSS kernel in QEMU ==="
    local manifest="build/${PRESET}/moss-artifacts.json"
    uv run scripts/run_qemu.py --manifest "${manifest}" "${REMAINING_ARGS[@]+"${REMAINING_ARGS[@]}"}"
}

do_test() {
    do_build
    echo ""
    echo "=== Running MOSS kernel tests in QEMU ==="
    local manifest="build/${PRESET}/moss-artifacts.json"
    uv run scripts/kernel_validation.py run --manifest "${manifest}" --guest-timeout 60 "${REMAINING_ARGS[@]+"${REMAINING_ARGS[@]}"}"
}

COMMAND="${1:-run}"
shift || true
parse_args "$@"
PRESET="${ARCH}-${BUILD_TYPE}"

case "${COMMAND}" in
    run)
        do_run
        ;;
    test)
        do_test
        ;;
    build)
        do_build
        ;;
    shell)
        exec /bin/bash
        ;;
    *)
        echo "Unknown command: ${COMMAND}"
        echo "Usage: entrypoint.sh {run|test|build|shell} [--arch arm64|x86_64|riscv] [extra-args...]"
        exit 1
        ;;
esac
