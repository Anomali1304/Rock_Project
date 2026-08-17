#!/usr/bin/env bash
# stages/80-release.sh — package flashable kernel zip, addon modules
# zip (if $ADDONS set), and an all-modules zip (every .ko this run
# built — vendor MTK + in-tree GKI + addons alike).

set -e
source "${ROOT_DIR}/release/anykernel.sh"
source "${ROOT_DIR}/release/package_modules.sh"
source "${ROOT_DIR}/release/package_all_modules.sh"
