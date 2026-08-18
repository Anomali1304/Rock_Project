# overclock_mt6789

Out-of-tree kernel module (**v7.1**) for MediaTek MT6789 (Helio G99) providing:

- **GPU overclock** — patches OPP idx0 of `gpufreq`'s `working_table` (+ `signed_table` when resolvable) via kprobe-resolved symbols; PLL frequency is resynced through the frequency-hopping controller (`mtk_fh_set_rate`), never a raw register write while the PLL is live
- **GPU commit-triggered PLL resync** — kretprobe on `gpufreq_commit()` detects when the stock driver actually commits into the patched index and queues the real PLL write on a dedicated workqueue, instead of writing synchronously (avoids racing a still-low voltage rail)
- **GED `current_freqency` resync** — best-effort: locates GED's own cached `g_working_table` pointer via `kallsyms_lookup_name()` and patches it in place, so `/sys/kernel/ged/hal/current_freqency` reflects the OC instead of going stale
- **CPU overclock** — patches `REG_FREQ_LUT_TABLE` idx0 (MMIO) and `policy->freq_table[0]` (RAM) per cluster, the same structures the stock `mtk-cpufreq-hw` driver reads
- **CPU idx0 quiesce-before-patch** — forces the cluster off idx0 (via a temporary `freq_qos` cap) and confirms via hardware (`REG_FREQ_PERF_STATE`) before rewriting a *live* LUT row; an earlier revision without this froze the device and triggered a watchdog reboot
- **Cooling-device countermeasure** — a 50ms guardian kthread + an Energy Model top-perf-state patch, both aimed at the same symptom: `cpufreq_cooling`/EM snapping `policy->max` back to stock during thermal transitions
- **CPU LUT diagnostic** — read-only cross-check of `policy->freq_table` (RAM) vs `REG_FREQ_LUT_TABLE` (MMIO) per cluster domain, no writes, no kprobes
- **CPU stats refresh** — debug-only, manually triggered: rebuilds a cluster's `time_in_state` bucket table via `kallsyms`-resolved `cpufreq_stats_free_table`/`create_table`, since `mtk-cpufreq-hw`'s policy is never torn down by CPU hotplug and the stats table otherwise stays frozen at its pre-OC snapshot until reboot
- **Suspend guard** — a PM notifier refuses OC apply operations while the device is suspending/hibernating, since MMIO/kprobe calls mid-suspend can hang the bus and trip the watchdog

**Ships DISABLED at runtime** — every OC parameter defaults to `0`/off. Enabling OC on-device is a separate, manual step. Flash/insmod at your own risk.

This is an addon of the **[Rock Project](../../README.md)** kernel build pipeline — see the top-level README and [`docs/ADDONS.md`](../../docs/ADDONS.md) for how addons are staged and compiled. It builds out-of-tree against a finished kernel workspace, either manually (see below) or automatically as part of a full `./build.sh` run with `ADDONS=overclock_mt6789` set, right after the kernel build finishes, against the matching `Module.symvers` and Clang toolchain.

## Hardware Details

| Item | Value |
|------|-------|
| SoC | MediaTek MT6789 (Helio G99) |
| Device | Xiaomi POCO M5 (codename `rock`) |
| Kernel | 5.10 GKI, `CONFIG_CFI_CLANG=y` |
| CPU driver | `mtk-cpufreq-hw` (`REG_FREQ_LUT_TABLE` MMIO + `policy->freq_table` RAM mirror) |
| GPU driver | `gpufreq` (`working_table` / `signed_table`) + GED (`g_working_table` cache) |
| GPU PLL | APMIXED base `0x1000C000`, `MFGPLL_CON1` offset `0x26C` |

### CPU Clusters

| Cluster | Representative CPU | Cores | Core Type |
|---------|--------------------|-------|-----------|
| Little (LL) | cpu0 | 0–3 | Cortex-A55 |
| Big (B) | cpu6 | 4–7 | Cortex-A76 |

Only 2 domains — MT6789 has no third/prime cluster.

### GPU OPP Entry Layout

```c
struct gpufreq_opp_info {
    unsigned int freq;    // KHz
    unsigned int volt;    // mV x 100 (e.g. 90000 = 900.0 mV)
    unsigned int vsram;   // mV x 100
    enum gpufreq_posdiv posdiv;
    unsigned int vaging;
    unsigned int power;
};
```

### CPU LUT Row

Only idx0 is ever written. `REG_FREQ_LUT_TABLE` row 0 is a read-modify-write on the frequency field only:

```
bits[11:0]  = Frequency (MHz), FIELD_PREP into GENMASK(11, 0)
```

**No per-row voltage field on this LUT** — SVS hardware handles voltage autonomously, calibrated only up to stock. Above-stock frequencies run on unguaranteed voltage extrapolation; there is no explicit voltage control on the CPU OC path at all. This is a materially different (and less controllable) situation than a CSRAM-based LUT with an explicit voltage field — treat the safety caps below as load-bearing, not a formality.

## Safety Caps

| Cap | Value | Notes |
|---|---|---|
| CPU max over stock | +60% (`MAX_OC_PERCENT_OVER_STOCK`) | |
| CPU absolute ceiling | 2600 MHz (`MAX_OC_ABSOLUTE_KHZ`) | Stricter of the two caps wins |
| GPU freq range | 375000–1900000 KHz | Posdiv-derived; out-of-range rejected |
| GPU volt range | 50000–129300 (mV×100) | Out-of-range rejected |

These are soft caps enforced in the module itself, not hardware limits — they don't guarantee stability, they just reject obviously unsafe values before a write happens.

## Countermeasures (why each one exists)

| Mechanism | Problem it addresses |
|---|---|
| `quiesce_off_idx0()` before every CPU LUT write | Rewriting a *live* idx0 row previously froze the device / triggered a watchdog reboot. Temporarily raises the QoS floor above idx0, polls `REG_FREQ_PERF_STATE` (500µs steps, 50ms timeout) until hardware confirms it left idx0, then patches. Times out safely (`-ETIMEDOUT`, nothing touched) rather than patching blind. |
| `patch_em_table()` | The Energy Model's top perf-state is a permanent stock snapshot that `cpufreq_cooling.c` prefers over `policy->freq_table` — without patching it too, cooling transitions snap `policy->max` back to stock. |
| Guardian kthread (50ms poll) | Something (most likely the cooling device's own standing `freq_qos` MAX request) keeps re-asserting stock `policy->max`. The guardian only re-applies the OC value when `policy->max` lands *exactly* on the stock value — real thermal throttling produces variable values and never hits that exact number, so this doesn't fight genuine thermal mitigation. |
| GPU PLL write deferred to a workqueue, triggered by a `gpufreq_commit()` kretprobe | Writing the PLL synchronously inside the sysfs `apply` call could race a still-low voltage rail set by the stock driver's own commit sequence. The kretprobe fires only once the driver has actually committed into a patched index, then the real PLL write happens out-of-line. |
| PCW-then-POSDIV / POSDIV-then-PCW ordering (`write_pll_con1_safe`) | A raw `writel()` to the PLL while live caused instant reboots. All frequency changes go through the frequency-hopping controller (`mtk_fh_set_rate`) for the PCW step; POSDIV and PCW are ordered opposite directions depending on scaling up vs down. |
| PM notifier (suspend guard) | MMIO/kprobe calls mid-suspend can hang the bus and trip the watchdog — any `_apply` write is refused outright while `PM_SUSPEND_PREPARE`/`PM_HIBERNATION_PREPARE` is active. |

## Sysfs Interface

All parameters are under `/sys/module/overclock_mt6789/parameters/`.

### GPU OC

| Parameter | Access | Description |
|-----------|--------|-------------|
| `gpu_target_freq` | RW | Target freq in KHz (`0` = off, max `1900000`) |
| `gpu_target_volt` | RW | Target voltage, mV×100 (e.g. `90000` = 900mV) |
| `gpu_target_vsram` | RW | Target vsram, mV×100 (`0` = same as `gpu_target_volt`) |
| `gpu_oc_apply` | W | Write `1` to patch OPP idx0 with the three values above |
| `gpu_oc_result` | R | Result string of last apply (`OK: ...` / `FAIL: ...`) |
| `gpu_opp_dump` | R | Full OPP table dump, patched entries marked `*` |

```bash
# Set GPU OPP idx0 to 1000MHz / 900mV, apply
echo 1000000 > /sys/module/overclock_mt6789/parameters/gpu_target_freq
echo 90000   > /sys/module/overclock_mt6789/parameters/gpu_target_volt
echo 1       > /sys/module/overclock_mt6789/parameters/gpu_oc_apply
cat /sys/module/overclock_mt6789/parameters/gpu_oc_result

# Disable GPU OC, restore stock
echo 0 > /sys/module/overclock_mt6789/parameters/gpu_target_freq
echo 1 > /sys/module/overclock_mt6789/parameters/gpu_oc_apply
```

### CPU OC

| Parameter | Access | Description |
|-----------|--------|-------------|
| `cpu_ll_target_khz` | RW | Little cluster idx0 target freq, KHz (`0` = leave alone) |
| `cpu_b_target_khz` | RW | Big cluster idx0 target freq, KHz (`0` = leave alone) |
| `cpu_oc_apply` | W | Write `1` to apply both targets |
| `cpu_oc_result` | R | Result string of last apply |
| `cpu_ll_rep_cpu` / `cpu_b_rep_cpu` | R | Representative CPU# per cluster domain (fixed: 0 / 6) |
| `cpu_lut_dump` | R | Read-only cross-check: `policy->freq_table` (RAM) vs `REG_FREQ_LUT_TABLE` (MMIO), per domain |
| `cpu_stats_refresh` | W (debug) | Write `1`=little / `2`=big to rebuild that cluster's `time_in_state` table so it reflects the patched idx0 freq without a reboot. Not called automatically from `cpu_oc_apply` — trigger manually, one cluster at a time |
| `cpu_stats_refresh` (read) | R | Result string of last refresh (`OK: ...` / `FAIL: ...`) |

```bash
# Set little cluster to 2100MHz, big cluster to 2500MHz, apply
echo 2100000 > /sys/module/overclock_mt6789/parameters/cpu_ll_target_khz
echo 2500000 > /sys/module/overclock_mt6789/parameters/cpu_b_target_khz
echo 1       > /sys/module/overclock_mt6789/parameters/cpu_oc_apply
cat /sys/module/overclock_mt6789/parameters/cpu_oc_result

# Verify RAM vs MMIO agree
cat /sys/module/overclock_mt6789/parameters/cpu_lut_dump

# Restore stock on both clusters
echo 0 > /sys/module/overclock_mt6789/parameters/cpu_ll_target_khz
echo 0 > /sys/module/overclock_mt6789/parameters/cpu_b_target_khz
echo 1 > /sys/module/overclock_mt6789/parameters/cpu_oc_apply
```

```bash
# Refresh time_in_state so it shows the OC'd frequency (no reboot needed)
# Try big cluster first — lower blast radius than little (cpu0 is the boot CPU)
echo 2 > /sys/module/overclock_mt6789/parameters/cpu_stats_refresh
cat /sys/module/overclock_mt6789/parameters/cpu_stats_refresh
dmesg | tail -20
cat /sys/devices/system/cpu/cpu6/cpufreq/stats/time_in_state

# Only after big cluster is confirmed clean, try little
echo 1 > /sys/module/overclock_mt6789/parameters/cpu_stats_refresh
```

## Verified Results

<!-- TODO: fill in with your own on-device numbers, e.g.:
| Metric | Stock | After OC |
|---|---|---|
| CPU LL idx0 | ... MHz | ... MHz |
| CPU B idx0 | ... MHz | ... MHz |
| GPU OPP[0] | ... MHz / ... mV | ... MHz / ... mV |
-->
_Not yet filled in — add your tested stable targets here once confirmed on-device._

## Known Limitations

- **No explicit CPU voltage control.** SVS handles voltage autonomously and is only calibrated up to stock frequency — above-stock is unguaranteed extrapolation. Raise `cpu_*_target_khz` in small steps and stress-test between each.
- **GED sync is best-effort.** If `g_working_table` isn't resolvable via `kallsyms_lookup_name()` (`CONFIG_KALLSYMS_ALL` off, `ged.ko` not loaded yet at module init, or a gpufreq v1 build), the module logs a warning and no-ops the GED sync silently — the hardware-level GPU OC still applies either way, only `/sys/kernel/ged/hal/current_freqency` stays stale.
- **`signed_table` patching is optional.** If `gpufreq_get_signed_table` doesn't resolve, only `working_table` gets patched; this doesn't block the normal success path.
- **`cpufreq_mtk_mirror` struct layout is reverse-engineered**, confirmed via `cpu_lut_dump` cross-checks against live hardware — not from an upstream/generic definition. A vendor driver update could shift field offsets silently.
- **CFI**: built with `CONFIG_CFI_CLANG=y`; every indirect call through a kprobe/kallsyms-resolved pointer is `__nocfi`.
- **`cpu_stats_refresh` is debug-only, not part of the OC apply path.** `cpufreq_stats_free_table`/`create_table` are confirmed matching this signature against this kernel's own `drivers/cpufreq/cpufreq_stats.c` (android12-5.10 branch), but like the other kallsyms-resolved symbols, a vendor tree update could shift behavior. Only `policy->rwsem` is held during the rebuild (matching what the cpufreq core itself holds on the offline/online path) — the module does not otherwise coordinate with the governor. `create_table()` seeds `last_index` from `policy->cur`, which the CPU OC path never updates through the normal transition notifier chain (it patches MMIO/RAM directly); the refresh writes `policy->cur` to the current target just before rebuilding to avoid a stale `last_index`, but if `cpu_*_target_khz` is `0` (OC not applied) this sync is skipped and `policy->cur` is left as whatever the core last set. Trigger one cluster at a time, big before little (little's rep CPU is `cpu0`, the boot CPU).

## Requirements

- A finished GKI kernel build tree (`Module.symvers` present) — this module compiles out-of-tree (`M=`) against it.
- The **exact same Clang** the kernel itself was built with. This kernel is `CONFIG_CFI_CLANG=y`: a toolchain mismatch produces CFI panics at runtime, not a build failure.
- `CONFIG_KALLSYMS_ALL=y` on the target kernel (on by default on most GKI kernels) for the GED resync and `mtk_fh_set_rate` resolution.

## Build

```sh
make KDIR=~/OSS/common WORKSPACE=~/OSS
```

The `Makefile` auto-detects the Clang toolchain from `$WORKSPACE/common/build.config.common`'s `CLANG_PREBUILT_BIN`, falling back to scanning `$WORKSPACE/prebuilts/clang/host/linux-x86/` if that's not found. Run `make check-clang` to see what it resolved to before building.

```sh
make install     # adb push + insmod + tail dmesg
make uninstall    # adb rmmod
```

Or as part of a full pipeline run — see [`docs/ADDONS.md`](../../docs/ADDONS.md):

```sh
ADDONS=overclock_mt6789 ./build.sh
```

## License

GPL-2.0 — see [`LICENSE`](../../LICENSE).
