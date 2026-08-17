<div align="center">

# 🪨 Rock Project

**CI-driven build pipeline for Xiaomi's official MT6789 (Helio G99) kernel source — born to ship the `overclock_mt6789` OC module, grown into a pluggable addon build system.**

[![Build](https://github.com/Anomali1304/Rock_Project/actions/workflows/build.yml/badge.svg)](https://github.com/Anomali1304/Rock_Project/actions/workflows/build.yml)
[![Version](https://img.shields.io/badge/version-1.0.0-informational?style=flat-square)](CHANGELOG.md)
[![Kernel](https://img.shields.io/badge/kernel-5.10%20(Xiaomi)-informational?style=flat-square)](docs/ARCHITECTURE.md)
[![License: GPL v2](https://img.shields.io/badge/license-GPL--2.0-blue?style=flat-square)](LICENSE)

Target device: Xiaomi POCO M5 (codename `rock`, MT6789/Helio G99) &middot; Android 12, kernel 5.10 &middot; source: [`android_kernel_xiaomi_rock`](https://github.com/Anomali1304/android_kernel_xiaomi_rock)

**[Quick Start](#quick-start) · [Run a Build ↗](https://github.com/Anomali1304/Rock_Project/actions) · [Project Layout](#project-layout) · [Addons](#addons) · [Config Reference](#configuration-reference) · [Architecture Docs](docs/ARCHITECTURE.md) · [Safety Notice](#safety-notice)**

</div>

---

## Table of contents

- [Overview](#overview)
- [Features](#features)
- [Requirements](#requirements)
- [Quick start](#quick-start)
- [Project layout](#project-layout)
- [CI/CD](#cicd)
- [Output](#output)
- [Addons](#addons)
- [Configuration reference](#configuration-reference)
- [Safety notice](#safety-notice)
- [License](#license)

## Overview

Rock Project started as a build pipeline for one thing: `overclock_mt6789`,
an out-of-tree CPU/GPU overclock module for the POCO M5's MT6789 SoC. It
grew into a full GitHub Actions build system for the underlying kernel
itself — Xiaomi's own official 5.10 source, **not** Google's generic
kernel — with `overclock_mt6789` as its first addon and reference
implementation for any addon that comes after it.

The GKI manifest is used only for build tooling (`build/build.sh`, prebuilt
toolchains, `build.config.*`); the kernel tree actually compiled is cloned
straight from Xiaomi's release repo. See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the exact split.

One script, one pipeline, every artifact every run:

```bash
./build.sh                                       # full build: kernel + addons (if set) + all modules
ADDONS=overclock_mt6789 ROOT_TYPE=KSUN ./build.sh # same, with an addon + KernelSU-Next root
```

## Features

- 🔀 **Root, your way** — Vanilla, KernelSU-Next, or ReSukiSU, each with optional SUSFS.
- 🧩 **Pluggable addons** — module source lives right here under `addons/<name>/`, no external repo or clone step; add a dir, it's buildable. Packaged into its own zip automatically when `$ADDONS` is set.
- 📦 **Every module, every run** — vendor MTK + in-tree GKI + addon `.ko` files are all zipped together too, no separate step needed.
- 🎛️ **One config file** — every knob lives in [`config/defaults.env`](config/defaults.env), documented inline.
- 🤖 **CI-native** — a single GitHub Actions workflow, `workflow_dispatch` with every knob exposed as an input.

## Requirements

- Linux host (or CI runner) with `bash`, `git`, `repo`, and a standard
  Android kernel toolchain — installed automatically by `stages/00-deps.sh`.
- ~30–60 GB free disk space for a full kernel sync + build.
- No manual setup beyond cloning this repo — every other dependency is
  resolved by the pipeline itself.

## Quick start

```bash
# Full build, defaults from config/defaults.env
./build.sh

# With GPU/CPU overclock addon, KernelSU-Next root
ADDONS=overclock_mt6789 ROOT_TYPE=KSUN ./build.sh

# Only grab a specific module from the all-modules zip
MODULES_FILTER=gpufreq ./build.sh

# Dry run — sync, patch, configure; skip compile and packaging
RUN_MODE="Dry Run" ./build.sh
```

Every option is an environment variable with a documented default in
[`config/defaults.env`](config/defaults.env) — export to override, nothing to edit.

## Project layout

```
Rock_Project/
├── build.sh               entrypoint — single pipeline, stage runner
├── lib/                   shared helpers + path resolution
├── config/                documented default configuration
├── stages/                numbered pipeline steps (50/70 stage+compile every addon generically)
├── kernel/
│   ├── core/              LTO + size/debug-info tuning
│   └── root/              per-root-solution integration
├── addons/
│   └── overclock_mt6789/  <name>.c + Makefile — real module source, right here.
│                           Add a new dir here to add a new addon, see docs/ADDONS.md
├── release/               packaging (flashable zip, addon zip, all-modules zip)
├── docs/                  architecture + addon-authoring guide
└── .github/workflows/     build.yml
```

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the full stage
breakdown and [`docs/ADDONS.md`](docs/ADDONS.md) for how to add a new addon.

## CI/CD

One `workflow_dispatch` workflow, `.github/workflows/build.yml`: every
knob (`root_type`, `lto_mode`, `addons`, `modules_filter`, etc.) is a
workflow input. `cache_workspace` caches the finished workspace (keyed
on kernel repo/branch/root type/LTO mode) purely to speed up a later
rerun with the *same* config — it never changes what gets built or
packaged.

## Output

Every run produces, in `release-out/`:

| File | When | Contents |
|---|---|---|
| `<zip_prefix><root_suffix>.zip` | always | Flashable AnyKernel3 zip — kernel image only. |
| `<zip_prefix><root_suffix>-addons.zip` | `$ADDONS` set | Enabled addons' compiled `.ko` files. |
| `<zip_prefix><root_suffix>-modules.zip` | always (if any `.ko` built) | Every `.ko` this run built — vendor MTK, in-tree GKI, and addons alike. Narrow it with `MODULES_FILTER`. |

## Addons

`overclock_mt6789` is the reason this pipeline exists — a GPU
(`working_table`) + CPU (`cpufreq-hw` LUT) overclock module for MT6789,
built out-of-tree so no kernel-tree patch is needed. It ships fully
disabled at runtime (every OC parameter defaults to `0`/off); enabling OC
on-device is a deliberate, manual, at-your-own-risk step. Full internals —
patch points, the GED `current_freqency` resync caveat, build/install
commands — live in its own [`addons/overclock_mt6789/README.md`](addons/overclock_mt6789/README.md).

Any further addon just needs a new `addons/<name>/` directory — the
staging/compile stages are generic and don't need touching. See
[`docs/ADDONS.md`](docs/ADDONS.md) for the layout convention.

| Addon | Target | Summary |
|---|---|---|
| [`overclock_mt6789`](addons/overclock_mt6789) | MT6789 | GPU + CPU overclock via kprobe-based patching of the stock GPU working-table and CPU cpufreq-hw LUT. |

## Configuration reference

| Variable | Default | Meaning |
|---|---|---|
| `RUN_MODE` | `Full Run` | `Full Run` or `Dry Run`. |
| `KERNEL_REPO` / `KERNEL_BRANCH` | see `config/defaults.env` | Kernel source to build. |
| `ROOT_TYPE` | `VANILLA` | `VANILLA` / `KSUN` / `RESUKISU`. |
| `LTO_MODE` | `THIN` | `NONE` / `THIN` / `FULL`. |
| `USE_EXT_MODULES` | `y` | Build vendor MTK kernel modules. |
| `STRIP_DEBUG` | `y` | Strip debug info / tracing configs. |
| `ADDONS` | *(empty)* | Comma-separated addon names, e.g. `overclock_mt6789`. |
| `MODULES_FILTER` | *(empty)* | Substring filter on the all-modules zip's filenames. |
| `CLANG_VENDOR` | `GKI` | `GKI` / `ZyC` / `Custom`. |
| `KERNEL_NAME` | `RockKernel` | `CONFIG_LOCALVERSION` suffix. |
| `ZIP_PREFIX` | `Rock-Kernel` | Output zip filename prefix. |

Full list with comments: [`config/defaults.env`](config/defaults.env).

## Safety notice

Modules built here (notably `overclock_mt6789`) patch live kernel structures and
hardware registers at runtime. They ship with overclocking disabled by
default — enabling it is a deliberate, manual, on-device step. Flashing
and loading these modules is done entirely at your own risk; see
[`docs/ADDONS.md`](docs/ADDONS.md) for the safety design, known
caveats, and hardware-specific limits.

## License

GPL-2.0, matching the kernel modules built here — see [`LICENSE`](LICENSE).

## Version

`1.0.0` — see [`CHANGELOG.md`](CHANGELOG.md).