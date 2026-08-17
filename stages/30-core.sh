#!/usr/bin/env bash
# stages/30-core.sh — LTO mode + size/debug-info optimizations.

set -e
source "${ROOT_DIR}/kernel/core/lto.sh"
source "${ROOT_DIR}/kernel/core/size_optimizations.sh"
