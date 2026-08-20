# Architecture

Rock Project builds Xiaomi's official kernel source for the MediaTek
MT6789 (Helio G99) `android_kernel_xiaomi_rock` tree, using Google's
GKI manifest and build tooling (`repo`, `build/build.sh`, prebuilt
toolchains) rather than Google's own generic common kernel — plus a
set of optional out-of-tree kernel module "addons" that ship alongside
it. See [stages/10-download.sh](../stages/10-download.sh): the AOSP
GKI manifest is synced only to obtain the build tooling and
`build.config.*` files; the actual kernel tree it builds is cloned
from `KERNEL_REPO`, Xiaomi's own source release.

## One pipeline, every artifact

`build.sh` runs a single, fixed pipeline — no mode switch, no
"reuse a previous workspace" fast path. Every run does a fresh repo
sync + kernel compile, then packages everything that came out of it:

- **Kernel** (always): the flashable AnyKernel3 zip.
- **Addon(s)** (when `$ADDONS` is set): staged + compiled against the
  same build, packaged into their own zip too.
- **Modules** (always): every `.ko` this run produced — vendor MTK,
  in-tree GKI (wifi/bt/gps/gpufreq, etc.), and addons alike — zipped
  together, optionally narrowed with `MODULES_FILTER` (filename
  substring).

All three are outputs of the *same* compile — nothing here re-zips
stale output from an earlier run, so what you get is always fresh.

In CI this is one workflow, `.github/workflows/build.yml`, which
uploads up to three artifacts per run (`rock-kernel-*`,
`rock-kernel-addons-*` if addons were set, `rock-kernel-modules-*`).
`cache_workspace` (an `actions/cache` entry keyed on the build
fingerprint and cache layout) is purely a speed optimization for reruns
with the same config. It caches repo metadata and toolchains, but never
the checked-out `workspace/common` worktree. The kernel source is always
checked out fresh, so a stale or cancelled Git worktree cannot break the
next run.

## Stage pipeline

```
stages/00-deps.sh            install host build deps (apt, repo launcher)
stages/10-download.sh        repo sync + clone kernel + vendor modules
stages/15-clang-vendor.sh    optional ZyC/Custom clang download (CLANG_VENDOR)
stages/20-branding.sh        LOCALVERSION, build user/host, /proc/version
stages/30-core.sh            -> kernel/core/lto.sh, size_optimizations.sh
stages/40-root-variant.sh    -> kernel/root/<root_type>/*.sh
stages/50-addons-stage.sh    validate + copy addons/<name>/ into $WORKSPACE (generic, per enabled addon)
stages/60-build-kernel.sh    invoke GKI's build/build.sh
stages/70-addons-compile.sh  compile each staged addon's .ko (generic, per enabled addon)
stages/80-release.sh         -> release/anykernel.sh, package_modules.sh, package_all_modules.sh
```

`RUN_MODE=Dry Run` stops right after `stages/60-build-kernel.sh` —
sync, config, and compile setup all run, but the actual compile and
every packaging stage are skipped.

## Directory layout

```
build.sh              entrypoint — single pipeline, stage runner
lib/functions.sh      shared helpers (logging, fingerprint, clang)
lib/paths.sh          path resolution (sourced after config)
config/defaults.env   documented default config (env always wins)
stages/               numbered pipeline steps, see above
kernel/core/          LTO + size/debug-info tuning
kernel/root/_shared/  strip-root + SUSFS logic shared by all root types
kernel/root/<type>/   root solution integration (vanilla, ksun, resukisu)
addons/<name>/        real module source (<name>.c + Makefile), right here —
                      see docs/ADDONS.md
release/              packaging: flashable zip, addon zip, all-modules zip
.github/workflows/    build.yml
```

## Config

All tunables live in `config/defaults.env`, documented inline, using
`:=` so any pre-set environment variable (a workflow input, a local
`export`) always wins without editing the file. See that file for the
full list.

## Output

Every run produces, in `release-out/`:

- `<zip_prefix><root_suffix>.zip` — flashable AnyKernel3 zip, kernel
  image only, no `.ko` modules.
- `<zip_prefix><root_suffix>-addons.zip` — enabled addons' compiled
  `.ko` files, absent if `$ADDONS` was empty.
- `<zip_prefix><root_suffix>-modules.zip` — every `.ko` built this
  run (vendor MTK + in-tree GKI + addons), absent if none were built
  or none matched `MODULES_FILTER`.
