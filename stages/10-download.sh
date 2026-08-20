#!/usr/bin/env bash
# stages/10-download.sh — repo init/sync, clone kernel + vendor modules.
# Kernel-mode only — addon-mode reuses whatever is already in $WORKSPACE.

set -e
cd "$WORKSPACE"

if [ ! -d ".repo" ]; then
    log "repo init (${GKI_MANIFEST_BRANCH})..."
    repo init --no-repo-verify \
        -u https://android.googlesource.com/kernel/manifest \
        -b "${GKI_MANIFEST_BRANCH}" \
        --repo-rev=main \
        --depth=1
else
    ok ".repo already present, skipping repo init."
fi

# workspace/.repo AND workspace/common are both restored from the GH
# Actions cache together (see .github/workflows/build.yml). Normally
# that's consistent, but a run that got killed mid-sync (e.g. the
# concurrency cancel-in-progress group cancelling an in-flight run)
# can still have its cache saved by actions/cache's post step with
# common/.git left in a half-checked-out state. repo then dies with
# "unsupported checkout state" and a plain retry can't fix it, because
# it's not a stale/mismatched ref — the worktree itself is broken.
#
# So validate common/'s git state for real (not just "does the .repo
# project dir exist") before trusting the cache. Since we overwrite
# common/ with a fresh AOSP checkout in the next step anyway, just
# move it aside first so repo sync always sees a clean path — we
# reuse it below as a local reference to speed the kernel git clone
# back up instead of losing it.
CACHED_COMMON=""
if [ -d "common" ] && { [ ! -d ".repo/projects/common.git" ] || ! git -C common rev-parse --is-inside-work-tree >/dev/null 2>&1; }; then
    warn "common/ present without a usable checkout (stale/corrupt cache) — moving aside for repo sync."
    rm -rf common.cache
    mv common common.cache
    CACHED_COMMON="$WORKSPACE/common.cache"
fi

log "repo sync..."
if ! repo sync -j"$(nproc)" --no-tags --no-clone-bundle --current-branch -f; then
    warn "repo sync failed — retrying once with --force-sync (clears any other stale project checkouts)."
    if ! repo sync -j"$(nproc)" --no-tags --no-clone-bundle --current-branch -f --force-sync; then
        warn "repo sync still failing — .repo state itself looks corrupt, wiping it and starting a clean sync."
        rm -rf .repo
        repo init --no-repo-verify \
            -u https://android.googlesource.com/kernel/manifest \
            -b "${GKI_MANIFEST_BRANCH}" \
            --repo-rev=main \
            --depth=1
        repo sync -j"$(nproc)" --no-tags --no-clone-bundle --current-branch -f
    fi
fi

if [ -d "common" ]; then
    for f in build.config.common build.config.aarch64; do
        if [ -f "common/$f" ]; then
            cp -f "common/$f" "$WORKSPACE/$f"
            ok "$f saved to $WORKSPACE/$f"
        else
            warn "common/$f not found — GKI sync may have changed structure."
        fi
    done

    warn "Removing AOSP-provided common/ before cloning ${KERNEL_REPO}..."
    rm -rf common
fi

CLONE_CMD=(git clone --depth=1)
if [ -n "${KERNEL_BRANCH:-}" ]; then
    CLONE_CMD+=(-b "$KERNEL_BRANCH")
fi
# Reuse the cache-restored clone (moved aside above) as a --reference so
# git can reuse local objects instead of re-downloading everything —
# falls back to a plain clone automatically if the reference is missing
# or unrelated (git ignores a --reference that shares no history).
if [ -n "$CACHED_COMMON" ] && [ -d "$CACHED_COMMON/.git" ]; then
    log "Reusing cached kernel clone as --reference to speed up download..."
    CLONE_CMD+=(--reference-if-able "$CACHED_COMMON")
fi
CLONE_CMD+=("$KERNEL_REPO" common)

log "Cloning kernel source: ${KERNEL_REPO} ${KERNEL_BRANCH:+(branch: $KERNEL_BRANCH)} -> common/"
"${CLONE_CMD[@]}"
rm -rf "$CACHED_COMMON"
ok "Kernel source ready at $KERNEL_DIR"

for f in build.config.common build.config.aarch64; do
    if [ -f "$WORKSPACE/$f" ]; then
        cp -f "$WORKSPACE/$f" "$KERNEL_DIR/$f"
        ok "$f restored to $KERNEL_DIR/$f"
    else
        error "$f not found in workspace — cannot proceed."
    fi
done

if [ "${USE_EXT_MODULES:-y}" = "y" ]; then
    log "Cloning vendor MTK kernel modules (branch: ${VENDOR_MODULES_BRANCH})..."
    mkdir -p "$(dirname "$VENDOR_MODULES_DIR")"
    rm -rf "$VENDOR_MODULES_DIR"
    git clone --depth=1 -b "$VENDOR_MODULES_BRANCH" "$VENDOR_MODULES_REPO" "$VENDOR_MODULES_DIR"
    ok "Vendor modules ready at $VENDOR_MODULES_DIR"
else
    log "USE_EXT_MODULES=n — disabling EXT_MODULES in build.config.rock"
    BUILD_ROCK="$KERNEL_DIR/build.config.rock"
    if [ -f "$BUILD_ROCK" ]; then
        sed -i '/^# Rock_Project: EXT_MODULES override/,+1d' "$BUILD_ROCK"
        printf '\n# Rock_Project: EXT_MODULES override\nEXT_MODULES=""\n' >> "$BUILD_ROCK"
    fi
fi

# Records the config this workspace was built with, so a rerun that
# restores it from cache (see .github/workflows/build.yml) can detect
# a mismatched fingerprint.
build_fingerprint > "$WORKSPACE/.fingerprint"
