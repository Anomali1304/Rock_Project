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

# Never let a cache-restored kernel/common worktree participate in repo sync.
# The repo metadata/object store remains cacheable, but the checked-out
# worktree is disposable. This permanently prevents a stale/cancelled
# common/.git checkout from poisoning the next run.
CACHED_COMMON=""
if [ -d "common" ]; then
    log "Quarantining cache-restored common/ before repo sync..."
    rm -rf common.cache
    mv common common.cache
    CACHED_COMMON="$WORKSPACE/common.cache"
fi

log "repo sync..."
if ! repo sync -j"$(nproc)" --no-tags --no-clone-bundle --current-branch -f; then
    warn "repo sync failed — retrying with --force-sync."
    if ! repo sync -j"$(nproc)" --no-tags --no-clone-bundle --current-branch -f --force-sync; then
        warn "repo sync still failing — cached repo metadata is unusable. Wiping .repo and rebuilding it cleanly."
        rm -rf .repo
        repo init --no-repo-verify \
            -u https://android.googlesource.com/kernel/manifest \
            -b "${GKI_MANIFEST_BRANCH}" \
            --repo-rev=main \
            --depth=1
        repo sync -j"$(nproc)" --no-tags --no-clone-bundle --current-branch -f --force-sync
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
# Do not use the quarantined AOSP common/ as a Git alternates/reference
# source. The AOSP manifest checkout and the Xiaomi kernel repository are
# different repositories, and deleting a reference repository after clone
# can leave .git/objects/info/alternates pointing at a missing path.

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

# Record the effective source/build configuration for diagnostics.
build_fingerprint > "$WORKSPACE/.fingerprint"
