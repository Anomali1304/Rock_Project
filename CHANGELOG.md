# Changelog

## 1.0.0

Initial release. Single-repo kernel build pipeline for Xiaomi's MT6789
(Helio G99) GKI source — kernel, root solutions (Vanilla / KSUN /
ReSukiSU), and addons all in one place.

- Root, your way: Vanilla, KernelSU-Next, or ReSukiSU, each with optional SUSFS.
- Addons: real module source lives in `addons/<name>/`, no external repo, no clone step. Add a dir, it's buildable.
- Every module, every run: vendor MTK + in-tree GKI + addon `.ko` files all zipped together.
- One config file: every knob in `config/defaults.env`.
- CI-native: single GitHub Actions `workflow_dispatch`, every knob exposed as an input.
