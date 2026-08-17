#!/usr/bin/env bash
# stages/40-root-variant.sh — dispatch to the chosen root solution.

set -e
script="${ROOT_DIR}/kernel/root/${ROOT_TYPE,,}/${ROOT_TYPE,,}.sh"
[ -f "$script" ] || error "Unknown ROOT_TYPE=${ROOT_TYPE} (no ${script})"
source "$script"
