#!/usr/bin/env bash
# kernel/root/vanilla/vanilla.sh — strip any root solution leftovers.

set -e
cd "$KERNEL_DIR"

source "${ROOT_DIR}/kernel/root/_shared/strip_root.sh"

ok "Root disabled (Vanilla)."
