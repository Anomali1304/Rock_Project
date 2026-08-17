# Writing an addon

An addon is an out-of-tree kernel module built and packaged alongside
the main kernel, without patching the kernel tree itself. Source lives
directly in this repo, under `addons/<name>/` — there's no separate
modules repo to clone, no manifest to fill in, and nothing outside
`addons/<name>/` to touch.

## Layout

```
addons/<name>/
├── <name>.c     module source — obj-m target must match this filename
├── Makefile     standard out-of-tree kbuild Makefile (see below)
└── README.md    optional, module-specific docs
```

That's it — no `addon.yaml`, no `stage.sh`, no `compile.sh`. The
pipeline's `stages/50-addons-stage.sh` and `stages/70-addons-compile.sh`
are generic: they loop over whatever's in `$ADDONS` and apply the same
logic to every name, driven purely by the directory/file naming
convention above. Adding a new addon never requires touching either
stage script.

**The one rule that matters:** the directory name, the `.c` filename
(minus `.c`), and the Makefile's `obj-m` target must all match. That's
what lets the generic stages find and build any addon without
addon-specific code.

```makefile
# addons/my_addon/Makefile
obj-m := my_addon.o
```

## Makefile requirements

Your `Makefile` just needs a standard `modules` default target that
builds against a `KDIR` passed in from outside — the pipeline invokes
it as:

```sh
make -C addons/<name> KDIR=<kernel build dir> ARCH=arm64 \
    CROSS_COMPILE=aarch64-linux-gnu- CC=<clang path> \
    CLANG_TRIPLE=aarch64-linux-gnu- LLVM=1 LLVM_IAS=1
```

See `addons/overclock_mt6789/Makefile` for a working reference — it
also auto-detects the matching Clang when run standalone (outside this
pipeline), which is handy for local testing but not required by the
pipeline itself (the pipeline always passes `CC` explicitly).

## What the pipeline does for you

- `stages/50-addons-stage.sh` (runs after kernel source is ready, before
  it's compiled): validates `addons/<name>/<name>.c` and `Makefile`
  exist, then copies the whole directory into `$WORKSPACE/addons/<name>/`.
  A missing or misnamed file fails loudly here — not deep inside a
  compile log.
- `stages/70-addons-compile.sh` (runs right after
  `stages/60-build-kernel.sh`, so `Module.symvers` already exists):
  locates `Module.symvers` (searched, not hardcoded — GKI's output
  layout shifts between branches), resolves the **exact** Clang the
  kernel itself was built with (a mismatch on this `CONFIG_CFI_CLANG=y`
  kernel means boot-time CFI panics, not just a build failure — never
  falls back to a bare `clang` off `$PATH`), builds with `LLVM=1
  LLVM_IAS=1` to match the kernel's own codegen, and copies the
  resulting `<name>.ko` to `$OUT_DIR/<name>/`.

## Enabling an addon

Set `ADDONS=<name>` (comma-separated for more than one) — as an
environment variable, a `workflow_dispatch` input, or in
`config/defaults.env`. The workflow's `addons` field is free text, so
a brand-new addon works the moment its directory lands in `addons/` on
the branch being built — no workflow file changes needed.

## Current addons

| Addon | Target | Summary |
|---|---|---|
| [`overclock_mt6789`](../addons/overclock_mt6789/) | MT6789 | GPU + CPU overclock via kprobe-based working-table/LUT patching. Ships disabled; enabling OC is a manual on-device step. |

---

Module-internal behavior, caveats, and safety notes for a given addon
(e.g. `overclock_mt6789`'s GED `current_freqency` sync quirk) live in
that addon's own `README.md` under `addons/<name>/` — this doc only
covers how the *pipeline* validates and compiles addons, not what any
one module does internally.
