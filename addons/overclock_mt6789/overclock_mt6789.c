// SPDX-License-Identifier: GPL-2.0
/*
 * overclock_mt6789.c — GPU + CPU overclock module for MT6789 (Helio G99).
 *
 * GPU: patches OPP idx0 in gpufreq's working_table (freq/volt/vsram) via
 * kprobe-resolved symbols, resyncs the PLL through mtk_fh_set_rate().
 * CPU: patches REG_FREQ_LUT_TABLE (MMIO) + policy->freq_table (RAM) idx0,
 * the same structures the stock mtk-cpufreq-hw driver uses.
 *
 * CONFIG_CFI_CLANG=y: any indirect call through a kprobe-resolved pointer
 * must be __nocfi or it panics on first invocation.
 *
 * Ships DISABLED — every OC parameter defaults to 0/off. Flash and load
 * at your own risk.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/atomic.h>
#include <linux/suspend.h>
#include <linux/string.h>
#include <linux/cpufreq.h>
#include <linux/bitfield.h>
#include <linux/pm_qos.h>
#include <linux/kthread.h>
#include <linux/energy_model.h>

/* MMIO/kprobe calls mid-suspend can hang the bus and trip the watchdog. */
static atomic_t oc_mt6789_suspended = ATOMIC_INIT(0);

static int oc_mt6789_pm_notifier(struct notifier_block *nb,
			       unsigned long action, void *data)
{
	switch (action) {
	case PM_SUSPEND_PREPARE:
	case PM_HIBERNATION_PREPARE:
		atomic_set(&oc_mt6789_suspended, 1);
		break;
	case PM_POST_SUSPEND:
	case PM_POST_HIBERNATION:
		atomic_set(&oc_mt6789_suspended, 0);
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block oc_mt6789_pm_nb = {
	.notifier_call = oc_mt6789_pm_notifier,
};

/* kallsyms_lookup_name is unexported on 5.10; resolve symbols via kprobe. */
static __nocfi int __kprobe_nop(struct kprobe *kp, struct pt_regs *regs)
{
	return 0;
}

static unsigned long resolve_ksym(const char *name)
{
	struct kprobe kp = {
		.symbol_name = name,
		.pre_handler = __kprobe_nop,
	};
	unsigned long addr;
	int ret = register_kprobe(&kp);

	if (ret < 0)
		return 0;
	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

/* ========== GPU OC — working_table patch + PLL resync ========== */
enum gpufreq_posdiv {
	POSDIV_POWER_1  = 0,
	POSDIV_POWER_2  = 1,
	POSDIV_POWER_4  = 2,
	POSDIV_POWER_8  = 3,
	POSDIV_POWER_16 = 4,
};
enum gpufreq_target { TARGET_STACK = 0, TARGET_GPU = 1, TARGET_DEFAULT = 2 };

struct gpufreq_opp_info {
	unsigned int        freq;   /* KHz */
	unsigned int        volt;   /* mV x 100 */
	unsigned int        vsram;  /* mV x 100 */
	enum gpufreq_posdiv posdiv;
	unsigned int        vaging;
	unsigned int        power;
};

typedef const struct gpufreq_opp_info *(*fn_get_wt_t)(enum gpufreq_target);
typedef int (*fn_get_opp_num_t)(enum gpufreq_target);

static fn_get_wt_t      sym_gpufreq_get_working_table;
static fn_get_opp_num_t sym_gpufreq_get_opp_num;
/* Optional mirror; if unresolved, working_table still gets patched. */
static fn_get_wt_t      sym_gpufreq_get_signed_table;

/* GED caches its own copy of the OPP table at boot, so current_freqency
 * goes stale unless resynced here too. g_working_table isn't
 * EXPORT_SYMBOL'd, so it's located via kallsyms_lookup_name()
 * (needs CONFIG_KALLSYMS_ALL=y). Only valid once GED's init has run —
 * always check *sym_ged_g_working_table for NULL before dereferencing. */
static struct gpufreq_opp_info **sym_ged_g_working_table;

/* Data symbol, not code — resolve via kallsyms, not kprobe. */
typedef unsigned long (*fn_kallsyms_lookup_name_t)(const char *name);
static fn_kallsyms_lookup_name_t sym_kallsyms_lookup_name;

typedef bool (*fn_mtk_fh_set_rate_t)(const char *name, unsigned long dds, int postdiv);
static fn_mtk_fh_set_rate_t sym_mtk_fh_set_rate;

/* drivers/cpufreq/cpufreq_stats.c — neither EXPORT_SYMBOL'd, resolved
 * via kallsyms below. Used only by the debug cpu_stats_refresh param. */
typedef void (*fn_stats_free_t)(struct cpufreq_policy *policy);
typedef void (*fn_stats_create_t)(struct cpufreq_policy *policy);
static fn_stats_free_t   sym_cpufreq_stats_free_table;
static fn_stats_create_t sym_cpufreq_stats_create_table;

#define MFG_PLL_NAME "mfgpll"

#define APMIXED_PHYS     0x1000C000UL
#define APMIXED_SIZE     0x1000UL
#define MFGPLL_CON1_OFF  0x26C

static void __iomem *g_apmixed_va;
#define MFGPLL_CON1  (g_apmixed_va + MFGPLL_CON1_OFF)

#define PLL_FIN        26U
#define DDS_SHIFT      14U
#define TO_MHZ_HEAD    100U
#define TO_MHZ_TAIL    10U
#define ROUNDING_VALUE 5U
#define POSDIV_SHIFT   24U

#define POSDIV_2_MAX_FREQ  1900000U  /* KHz */
#define POSDIV_4_MIN_FREQ  375000U
#define VGPU_MAX_VOLT      129300U   /* mV*100 */
#define VGPU_MIN_VOLT      50000U

#define GPU_OPP_MAX 72  /* headroom above known ~65-entry tables on this SoC family */

static unsigned int gpu_target_freq  = 0;
static unsigned int gpu_target_volt  = 0;
static unsigned int gpu_target_vsram = 0;
static int          gpu_oc_apply     = 0;
static char gpu_oc_result[128] = "not applied";
static char gpu_opp_dump[4096] = "";

module_param(gpu_target_freq,  uint, 0644);
MODULE_PARM_DESC(gpu_target_freq,  "GPU target freq KHz (0=off, max 1900000)");
module_param(gpu_target_volt,  uint, 0644);
MODULE_PARM_DESC(gpu_target_volt,  "GPU target volt mV*100 (e.g. 90000=900mV)");
module_param(gpu_target_vsram, uint, 0644);
MODULE_PARM_DESC(gpu_target_vsram, "GPU vsram mV*100 (0=same as volt)");
module_param_string(gpu_oc_result, gpu_oc_result, sizeof(gpu_oc_result), 0444);
MODULE_PARM_DESC(gpu_oc_result, "GPU OC status (read-only)");
/* gpu_opp_dump itself is registered as a live module_param_cb further down,
 * right after update_gpu_opp_dump() is defined — see gpu_opp_dump_get(). */

static DEFINE_MUTEX(oc_lock);

/* Only idx0 is ever written, but the array lets restore-to-stock work
 * correctly regardless of history. */
static bool                     g_gpu_patched[GPU_OPP_MAX];
static struct gpufreq_opp_info  g_gpu_orig[GPU_OPP_MAX];
static atomic_t                 g_last_commit_idx = ATOMIC_INIT(-1);
static unsigned int             g_last_pll_freq;
static bool                     g_gpu_oc_active;
static bool                     g_hw_mapped;

static struct workqueue_struct *g_oc_wq;
static struct delayed_work      g_gpu_pll_work;

static unsigned int calc_pcw(unsigned int freq_khz, enum gpufreq_posdiv posdiv)
{
	return (((freq_khz / TO_MHZ_HEAD * (1u << (unsigned)posdiv)) << DDS_SHIFT)
		/ PLL_FIN + ROUNDING_VALUE) / TO_MHZ_TAIL;
}

static enum gpufreq_posdiv freq_to_posdiv_gpu(unsigned int freq_khz)
{
	if (freq_khz > 950000)  return POSDIV_POWER_2;
	if (freq_khz > 475000)  return POSDIV_POWER_4;
	if (freq_khz > 237500)  return POSDIV_POWER_8;
	return POSDIV_POWER_16;
}

/* PCW always goes through the frequency-hopping controller, never a raw
 * writel() while the PLL is live — caused instant reboots otherwise. */
static __nocfi bool write_pll_con1_safe(void __iomem *reg, const char *pll_name,
				 unsigned int freq_old_khz, unsigned int freq_new_khz)
{
	enum gpufreq_posdiv target_posdiv;
	unsigned int pcw, pll;

	if (!reg || !sym_mtk_fh_set_rate)
		return false;

	target_posdiv = freq_to_posdiv_gpu(freq_new_khz);
	pcw = calc_pcw(freq_new_khz, target_posdiv);
	if (!pcw)
		return false;

	if (freq_new_khz > freq_old_khz) {
		/* scale up: PCW (hopping) first, then POSDIV */
		if (!sym_mtk_fh_set_rate(pll_name, pcw, target_posdiv))
			return false;
		pll = (readl(reg) & 0xF8FFFFFFu) | ((unsigned)target_posdiv << POSDIV_SHIFT);
		writel(pll, reg);
		udelay(20);
	} else {
		/* scale down: POSDIV first, then PCW (hopping) */
		pll = (readl(reg) & 0xF8FFFFFFu) | ((unsigned)target_posdiv << POSDIV_SHIFT);
		writel(pll, reg);
		udelay(20);
		if (!sym_mtk_fh_set_rate(pll_name, pcw, target_posdiv))
			return false;
	}
	return true;
}

/* Runs via workqueue, triggered by the gpufreq_commit kretprobe once
 * the real driver commits into a patched index. */
static __nocfi void gpu_pll_work_fn(struct work_struct *work)
{
	struct gpufreq_opp_info *wt;
	unsigned int freq_old, freq_new;
	int idx;

	if (atomic_read(&oc_mt6789_suspended))
		return;
	if (!g_gpu_oc_active || !g_apmixed_va || !sym_gpufreq_get_working_table)
		return;

	idx = atomic_read(&g_last_commit_idx);
	if (idx < 0 || idx >= GPU_OPP_MAX || !g_gpu_patched[idx])
		return;

	wt = (struct gpufreq_opp_info *)sym_gpufreq_get_working_table(TARGET_GPU);
	if (!wt)
		return;

	freq_new = wt[idx].freq;
	freq_old = g_last_pll_freq ? g_last_pll_freq : g_gpu_orig[idx].freq;

	if (write_pll_con1_safe(MFGPLL_CON1, MFG_PLL_NAME, freq_old, freq_new))
		g_last_pll_freq = freq_new;
	else
		snprintf(gpu_oc_result, sizeof(gpu_oc_result),
			 "FAIL: PLL write refused (idx=%d) — OC not applied to PLL", idx);
}

static __nocfi int gpu_patch_one_index(struct gpufreq_opp_info *wt,
					struct gpufreq_opp_info *st,
					int n, int idx,
					unsigned int freq, unsigned int volt,
					unsigned int vsram)
{
	if (idx < 0 || idx >= n || idx >= GPU_OPP_MAX)
		return -EINVAL;

	if (!g_gpu_patched[idx]) {
		g_gpu_orig[idx] = wt[idx];
		g_gpu_patched[idx] = true;
	}

	wt[idx].freq   = freq;
	wt[idx].volt   = volt;
	wt[idx].vsram  = vsram;
	wt[idx].posdiv = freq_to_posdiv_gpu(freq);

	if (st) {
		st[idx].freq   = freq;
		st[idx].volt   = volt;
		st[idx].vsram  = vsram;
		st[idx].posdiv = freq_to_posdiv_gpu(freq);
	}
	return 0;
}

/* Best-effort GED table resync; no-ops quietly if unresolved. */
static __nocfi void ged_snapshot_resync(int idx, const struct gpufreq_opp_info *val)
{
	if (!sym_ged_g_working_table || !*sym_ged_g_working_table)
		return;

	(*sym_ged_g_working_table)[idx] = *val;
}

/* Always patches idx0. PLL write is deferred to gpu_pll_work_fn() once
 * the driver commits a real transition — writing it synchronously here
 * could race a still-low voltage rail. */
static __nocfi int gpu_patch_working_table(unsigned int freq, unsigned int volt,
			   unsigned int vsram)
{
	struct gpufreq_opp_info *wt, *st = NULL;
	int n, ret;

	if (!sym_gpufreq_get_working_table || !sym_gpufreq_get_opp_num) {
		snprintf(gpu_oc_result, sizeof(gpu_oc_result), "FAIL: GPU symbols not resolved");
		return -ENODEV;
	}

	n  = sym_gpufreq_get_opp_num(TARGET_GPU);
	wt = (struct gpufreq_opp_info *)sym_gpufreq_get_working_table(TARGET_GPU);
	if (sym_gpufreq_get_signed_table)
		st = (struct gpufreq_opp_info *)sym_gpufreq_get_signed_table(TARGET_GPU);

	if (!wt || n <= 0) {
		snprintf(gpu_oc_result, sizeof(gpu_oc_result), "FAIL: table NULL or opp_num=%d", n);
		return -ENODATA;
	}

	ret = gpu_patch_one_index(wt, st, n, 0, freq, volt, vsram);
	if (ret) {
		snprintf(gpu_oc_result, sizeof(gpu_oc_result), "FAIL: idx0 patch rejected (%d)", ret);
		return ret;
	}
	ged_snapshot_resync(0, &wt[0]);
	snprintf(gpu_oc_result, sizeof(gpu_oc_result),
		 "OK: idx0 freq=%u volt=%u vsram=%u (n=%d, signed=%s, ged_sync=%s)",
		 freq, volt, vsram, n, st ? "yes" : "no",
		 (sym_ged_g_working_table && *sym_ged_g_working_table) ? "yes" : "no");
	return 0;
}

static __nocfi void gpu_restore_working_table(void)
{
	struct gpufreq_opp_info *wt, *st = NULL;
	int i, n;

	if (!sym_gpufreq_get_working_table || !sym_gpufreq_get_opp_num)
		return;
	wt = (struct gpufreq_opp_info *)sym_gpufreq_get_working_table(TARGET_GPU);
	if (!wt)
		return;
	if (sym_gpufreq_get_signed_table)
		st = (struct gpufreq_opp_info *)sym_gpufreq_get_signed_table(TARGET_GPU);
	n = sym_gpufreq_get_opp_num(TARGET_GPU);

	for (i = 0; i < GPU_OPP_MAX && i < n; i++) {
		if (!g_gpu_patched[i])
			continue;
		wt[i] = g_gpu_orig[i];
		if (st)
			st[i] = g_gpu_orig[i];
		ged_snapshot_resync(i, &g_gpu_orig[i]);
		g_gpu_patched[i] = false;
	}
	atomic_set(&g_last_commit_idx, -1);
	g_last_pll_freq = 0;
}

static __nocfi void update_gpu_opp_dump(void)
{
	struct gpufreq_opp_info *wt;
	int i, n, off = 0;

	if (!sym_gpufreq_get_working_table || !sym_gpufreq_get_opp_num) {
		snprintf(gpu_opp_dump, sizeof(gpu_opp_dump), "symbols not ready");
		return;
	}
	n  = sym_gpufreq_get_opp_num(TARGET_GPU);
	wt = (struct gpufreq_opp_info *)sym_gpufreq_get_working_table(TARGET_GPU);
	if (!wt || n <= 0) { snprintf(gpu_opp_dump, sizeof(gpu_opp_dump), "unavailable"); return; }

	off = snprintf(gpu_opp_dump, sizeof(gpu_opp_dump), "GPU OPP (n=%d):\n", n);
	for (i = 0; i < n && off < (int)sizeof(gpu_opp_dump) - 60; i++)
		off += snprintf(gpu_opp_dump + off, sizeof(gpu_opp_dump) - off,
			"[%2d]%s %7u KHz %7u mV*100 vsram=%7u\n",
			i, (i < GPU_OPP_MAX && g_gpu_patched[i]) ? "*" : " ",
			wt[i].freq, wt[i].volt, wt[i].vsram);
}

/* Live read: recompute on every sysfs read instead of relying on a stale
 * snapshot from the last apply. Mirrors cpu_lut_dump's module_param_cb
 * pattern below, so the GPU dump shows current state immediately after
 * insmod, before any OC has ever been applied. */
static int gpu_opp_dump_get(char *buf, const struct kernel_param *kp)
{
	mutex_lock(&oc_lock);
	update_gpu_opp_dump();
	mutex_unlock(&oc_lock);
	return scnprintf(buf, PAGE_SIZE, "%s", gpu_opp_dump);
}
static const struct kernel_param_ops gpu_opp_dump_ops = { .get = gpu_opp_dump_get };
module_param_cb(gpu_opp_dump, &gpu_opp_dump_ops, NULL, 0444);
MODULE_PARM_DESC(gpu_opp_dump, "READ-ONLY: live GPU OPP table dump (freq/volt/vsram), recomputed on every read");

/* kretprobe on gpufreq_commit(): kicks the PLL resync workqueue when
 * the driver commits into a patched index. */
static struct kretprobe krp_gpufreq_commit;

struct gpu_commit_args {
	enum gpufreq_target target;
	int                 oppidx;
};

static __nocfi int krp_gpufreq_commit_entry(struct kretprobe_instance *ri,
					    struct pt_regs *regs)
{
	struct gpu_commit_args *a = (struct gpu_commit_args *)ri->data;
	/* AAPCS64: arg0 -> x0, arg1 -> x1 */
	a->target = (enum gpufreq_target)(regs->regs[0] & 0xFFFFFFFFu);
	a->oppidx = (int)(regs->regs[1] & 0xFFFFFFFFu);
	return 0;
}

static __nocfi int krp_gpufreq_commit_ret(struct kretprobe_instance *ri,
					  struct pt_regs *regs)
{
	struct gpu_commit_args *a = (struct gpu_commit_args *)ri->data;
	bool is_patched_idx = (a->oppidx >= 0 && a->oppidx < GPU_OPP_MAX &&
				g_gpu_patched[a->oppidx]);

	if (atomic_read(&oc_mt6789_suspended))
		return 0;
	if (g_gpu_oc_active && g_oc_wq && a->target == TARGET_GPU && is_patched_idx) {
		atomic_set(&g_last_commit_idx, a->oppidx);
		queue_delayed_work(g_oc_wq, &g_gpu_pll_work, 0);
	}
	return 0;
}

/* Deferred to first real apply, so plain insmod never touches MMIO. */
static int lazy_map_hw(void)
{
	if (g_hw_mapped)
		return 0;

	g_apmixed_va = ioremap(APMIXED_PHYS, APMIXED_SIZE);
	if (!g_apmixed_va)
		pr_warn("oc_mt6789: apmixed ioremap failed -> GPU PLL write disabled\n");

	g_hw_mapped = true;
	return g_apmixed_va ? 0 : -ENODEV;
}

static int gpu_oc_apply_set(const char *val, const struct kernel_param *kpp)
{
	int v, ret;

	ret = kstrtoint(val, 0, &v);
	if (ret || v != 1)
		return ret;

	if (atomic_read(&oc_mt6789_suspended)) {
		snprintf(gpu_oc_result, sizeof(gpu_oc_result), "FAIL: device suspending, refused");
		gpu_oc_apply = 0;
		return 0;
	}

	mutex_lock(&oc_lock);

	if (lazy_map_hw() < 0) {
		snprintf(gpu_oc_result, sizeof(gpu_oc_result), "FAIL: hardware mapping failed");
		goto out;
	}

	if (gpu_target_freq == 0) {
		cancel_delayed_work_sync(&g_gpu_pll_work);
		gpu_restore_working_table();
		g_gpu_oc_active = false;
		snprintf(gpu_oc_result, sizeof(gpu_oc_result), "OK: GPU OC disabled, stock restored");
		goto out;
	}

	if (gpu_target_freq < POSDIV_4_MIN_FREQ || gpu_target_freq > POSDIV_2_MAX_FREQ) {
		snprintf(gpu_oc_result, sizeof(gpu_oc_result),
			 "FAIL: freq %u out of [%u, %u] KHz",
			 gpu_target_freq, POSDIV_4_MIN_FREQ, POSDIV_2_MAX_FREQ);
		goto out;
	}
	if (gpu_target_volt < VGPU_MIN_VOLT || gpu_target_volt > VGPU_MAX_VOLT) {
		snprintf(gpu_oc_result, sizeof(gpu_oc_result),
			 "FAIL: volt %u out of [%u, %u] mV*100",
			 gpu_target_volt, VGPU_MIN_VOLT, VGPU_MAX_VOLT);
		goto out;
	}
	if (gpu_target_vsram == 0)
		gpu_target_vsram = gpu_target_volt;

	ret = gpu_patch_working_table(gpu_target_freq, gpu_target_volt, gpu_target_vsram);
	if (ret)
		goto out;

	g_gpu_oc_active = true;
	update_gpu_opp_dump();

out:
	gpu_oc_apply = 0;
	mutex_unlock(&oc_lock);
	return 0;
}
static int gpu_oc_apply_get(char *buf, const struct kernel_param *kpp)
{
	return scnprintf(buf, PAGE_SIZE, "0\n");
}
static const struct kernel_param_ops gpu_oc_apply_ops = {
	.set = gpu_oc_apply_set,
	.get = gpu_oc_apply_get,
};
module_param_cb(gpu_oc_apply, &gpu_oc_apply_ops, &gpu_oc_apply, 0644);
MODULE_PARM_DESC(gpu_oc_apply, "Write 1 to apply gpu_target_freq/volt/vsram to OPP idx0");

/* CPU OC: patch LUT[0], cpufreq table[0], policy max and EM top state. */
#define LUT_MAX_ENTRIES   32U
#define LUT_FREQ          GENMASK(11, 0)
#define LUT_ROW_SIZE      0x4

enum {
	REG_FREQ_LUT_TABLE,
	REG_FREQ_ENABLE,
	REG_FREQ_PERF_STATE,
	REG_FREQ_HW_STATE,
	REG_EM_POWER_TBL,
	REG_FREQ_LATENCY,
	REG_ARRAY_SIZE,
};

struct cpufreq_mtk_mirror {
	struct cpufreq_frequency_table *table;
	void __iomem *reg_bases[REG_ARRAY_SIZE];
	int nr_opp;
	cpumask_t related_cpus;
};

static unsigned int cpu_ll_rep_cpu = 0;
static unsigned int cpu_b_rep_cpu  = 6;
module_param(cpu_ll_rep_cpu, uint, 0444);
module_param(cpu_b_rep_cpu, uint, 0444);
MODULE_PARM_DESC(cpu_ll_rep_cpu, "Representative CPU# for little cluster domain");
MODULE_PARM_DESC(cpu_b_rep_cpu, "Representative CPU# for big cluster domain");

#define MAX_OC_PERCENT_OVER_STOCK  60

#define MAX_OC_ABSOLUTE_KHZ  2600000U

static unsigned int cpu_ll_target_khz = 0;
static unsigned int cpu_b_target_khz  = 0;
static int          cpu_oc_apply      = 0;
module_param(cpu_ll_target_khz, uint, 0644);
MODULE_PARM_DESC(cpu_ll_target_khz, "Little cluster idx0 target freq in KHz (0=leave alone)");
module_param(cpu_b_target_khz, uint, 0644);
MODULE_PARM_DESC(cpu_b_target_khz, "Big cluster idx0 target freq in KHz (0=leave alone)");

static char cpu_oc_result[256] = "not applied yet";
static int cpu_oc_result_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", cpu_oc_result);
}
static const struct kernel_param_ops cpu_oc_result_ops = { .get = cpu_oc_result_get };
module_param_cb(cpu_oc_result, &cpu_oc_result_ops, NULL, 0444);
MODULE_PARM_DESC(cpu_oc_result, "CPU OC status (read-only)");

static unsigned int g_ll_orig_khz, g_b_orig_khz;
static bool         g_ll_have_orig, g_b_have_orig;

#define QUIESCE_POLL_US     500U
#define QUIESCE_TIMEOUT_US  50000U

static int quiesce_off_idx0(struct cpufreq_policy *policy, struct cpufreq_mtk_mirror *c,
			     struct freq_qos_request *qos_req)
{
	unsigned int waited_us = 0;
	int ret;

	if (c->nr_opp < 2)
		return -ENODATA;

	ret = freq_qos_add_request(&policy->constraints, qos_req, FREQ_QOS_MAX,
				    policy->freq_table[1].frequency);
	if (ret < 0)
		return ret;

	while (waited_us < QUIESCE_TIMEOUT_US) {
		if (readl_relaxed(c->reg_bases[REG_FREQ_PERF_STATE]) != 0)
			return 0;

		usleep_range(QUIESCE_POLL_US, QUIESCE_POLL_US * 2);
		waited_us += QUIESCE_POLL_US;
	}

	freq_qos_remove_request(qos_req);
	return -ETIMEDOUT;
}

static int patch_em_table(unsigned int rep_cpu, unsigned int use_khz)
{
	struct em_perf_domain *pd;
	struct em_perf_state *top;

	pd = em_cpu_get(rep_cpu);
	if (!pd || !pd->table || !pd->nr_perf_states)
		return -ENODEV;

	top = &pd->table[pd->nr_perf_states - 1];
	if (top->frequency == use_khz)
		return 0;

	top->frequency = use_khz;
	return 0;
}

static int patch_cluster_idx0(unsigned int rep_cpu, unsigned int target_khz,
			       unsigned int *orig_khz, bool *have_orig)
{
	struct cpufreq_policy *policy;
	struct cpufreq_mtk_mirror *c;
	struct freq_qos_request qos_req;
	unsigned int use_khz, cap_khz;
	u32 raw;
	int ret;

	policy = cpufreq_cpu_get(rep_cpu);
	if (!policy)
		return -ENODEV;

	if (!policy->driver_data || !policy->freq_table) {
		cpufreq_cpu_put(policy);
		return -ENODATA;
	}
	c = (struct cpufreq_mtk_mirror *)policy->driver_data;

	if (!*have_orig) {
		*orig_khz = policy->freq_table[0].frequency;
		*have_orig = true;
	}

	if (target_khz == 0) {
		use_khz = *orig_khz;
	} else {
		cap_khz = *orig_khz + (*orig_khz * MAX_OC_PERCENT_OVER_STOCK) / 100;
		if (cap_khz > MAX_OC_ABSOLUTE_KHZ)
			cap_khz = MAX_OC_ABSOLUTE_KHZ;
		if (target_khz > cap_khz) {
			cpufreq_cpu_put(policy);
			return -ERANGE;
		}
		use_khz = target_khz;
	}

	ret = quiesce_off_idx0(policy, c, &qos_req);
	if (ret) {
		cpufreq_cpu_put(policy);
		return ret;
	}

	raw = readl_relaxed(c->reg_bases[REG_FREQ_LUT_TABLE] + (0 * LUT_ROW_SIZE));
	raw = (raw & ~LUT_FREQ) | FIELD_PREP(LUT_FREQ, use_khz / 1000);
	writel_relaxed(raw, c->reg_bases[REG_FREQ_LUT_TABLE] + (0 * LUT_ROW_SIZE));

	policy->freq_table[0].frequency = use_khz;

	freq_qos_remove_request(&qos_req);

	policy->cpuinfo.max_freq = use_khz;
	policy->max = use_khz;
	if (policy->min > use_khz)
		policy->min = use_khz;

	if (patch_em_table(rep_cpu, use_khz))
		pr_warn("oc_mt6789: cpu%u has no EM perf domain — cooling device may still pin stock max\n",
			rep_cpu);

	cpufreq_cpu_put(policy);
	return 0;
}

static int cpu_oc_apply_set(const char *val, const struct kernel_param *kp)
{
	unsigned int trigger;
	int ret_ll = 0, ret_b = 0;

	if (kstrtouint(val, 10, &trigger))
		return -EINVAL;
	if (trigger != 1)
		return 0;

	if (atomic_read(&oc_mt6789_suspended)) {
		snprintf(cpu_oc_result, sizeof(cpu_oc_result),
			 "FAIL: device suspending, refused");
		return 0;
	}

	mutex_lock(&oc_lock);

	if (cpu_ll_target_khz || g_ll_have_orig)
		ret_ll = patch_cluster_idx0(cpu_ll_rep_cpu, cpu_ll_target_khz,
					     &g_ll_orig_khz, &g_ll_have_orig);
	if (cpu_b_target_khz || g_b_have_orig)
		ret_b = patch_cluster_idx0(cpu_b_rep_cpu, cpu_b_target_khz,
					    &g_b_orig_khz, &g_b_have_orig);

	if (ret_ll == -ERANGE || ret_b == -ERANGE)
		snprintf(cpu_oc_result, sizeof(cpu_oc_result),
			 "FAIL: target exceeds OC limit (ll=%u b=%u KHz)",
			 cpu_ll_target_khz, cpu_b_target_khz);
	else if (ret_ll == -ETIMEDOUT || ret_b == -ETIMEDOUT)
		snprintf(cpu_oc_result, sizeof(cpu_oc_result),
			 "FAIL: cluster would not leave idx0 within %ums",
			 QUIESCE_TIMEOUT_US / 1000);
	else if (ret_ll || ret_b)
		snprintf(cpu_oc_result, sizeof(cpu_oc_result),
			 "FAIL: ll=%d b=%d", ret_ll, ret_b);
	else
		snprintf(cpu_oc_result, sizeof(cpu_oc_result),
			 "OK: ll_max=%uKHz b_max=%uKHz (orig ll=%u b=%u)",
			 cpu_ll_target_khz ? cpu_ll_target_khz : g_ll_orig_khz,
			 cpu_b_target_khz ? cpu_b_target_khz : g_b_orig_khz,
			 g_ll_orig_khz, g_b_orig_khz);

	mutex_unlock(&oc_lock);
	return 0;
}

static int cpu_oc_apply_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "0\n");
}
static const struct kernel_param_ops cpu_oc_apply_ops = {
	.set = cpu_oc_apply_set,
	.get = cpu_oc_apply_get,
};
module_param_cb(cpu_oc_apply, &cpu_oc_apply_ops, &cpu_oc_apply, 0644);
MODULE_PARM_DESC(cpu_oc_apply, "Write 1 to apply cpu_ll_target_khz/cpu_b_target_khz to idx0");

#define GUARDIAN_POLL_MS  50

static struct task_struct *g_guardian_thread;

static void guardian_check_one(unsigned int rep_cpu, unsigned int target_khz,
				unsigned int orig_khz, bool have_orig)
{
	struct cpufreq_policy *policy;

	if (!have_orig || target_khz == 0)
		return;

	policy = cpufreq_cpu_get(rep_cpu);
	if (!policy)
		return;

	if (policy->max == orig_khz) {
		policy->max = target_khz;
		if (policy->cpuinfo.max_freq == orig_khz)
			policy->cpuinfo.max_freq = target_khz;
		if (policy->min > target_khz)
			policy->min = target_khz;
	}

	cpufreq_cpu_put(policy);
}

static int guardian_thread_fn(void *unused)
{
	while (!kthread_should_stop()) {
		mutex_lock(&oc_lock);
		guardian_check_one(cpu_ll_rep_cpu, cpu_ll_target_khz, g_ll_orig_khz, g_ll_have_orig);
		guardian_check_one(cpu_b_rep_cpu, cpu_b_target_khz, g_b_orig_khz, g_b_have_orig);
		mutex_unlock(&oc_lock);
		msleep_interruptible(GUARDIAN_POLL_MS);
	}
	return 0;
}

/* ===== CPU LUT DIAGNOSTIC — read-only, no writes, no kprobes =====
 * Cross-checks policy->freq_table (RAM) against REG_FREQ_LUT_TABLE
 * (MMIO), per cluster domain. Safe to read at any time. */
#define DUMP_BUF_SIZE 4096
static char g_dump_buf[DUMP_BUF_SIZE];
static int probe_cpus[] = {0, 1, 2, 3, 4, 5, 6, 7};

static void dump_one_cpu(int cpu, char *buf, size_t *off, size_t bufsize,
			  void *seen_tables[], int *n_seen)
{
	struct cpufreq_policy *policy;
	struct cpufreq_mtk_mirror *c;
	unsigned int cur_idx;
	int i;

	policy = cpufreq_cpu_get(cpu);
	if (!policy)
		return;

	if (!policy->driver_data || !policy->freq_table) {
		*off += scnprintf(buf + *off, bufsize - *off,
			"cpu%d: no driver_data/freq_table (driver != mtk-cpufreq-hw?)\n", cpu);
		cpufreq_cpu_put(policy);
		return;
	}

	/* skip if this domain (same freq_table) was already dumped */
	for (i = 0; i < *n_seen; i++) {
		if (seen_tables[i] == (void *)policy->freq_table) {
			cpufreq_cpu_put(policy);
			return;
		}
	}
	seen_tables[*n_seen] = (void *)policy->freq_table;
	(*n_seen)++;

	c = (struct cpufreq_mtk_mirror *)policy->driver_data;
	cur_idx = readl_relaxed(c->reg_bases[REG_FREQ_PERF_STATE]);

	*off += scnprintf(buf + *off, bufsize - *off,
		"\n=== cpu%d domain (nr_opp=%d, cur_idx=%u) ===\n", cpu, c->nr_opp, cur_idx);
	*off += scnprintf(buf + *off, bufsize - *off,
		"[idx] sw_freq(RAM)   hw_freq(MMIO)   match?\n");

	for (i = 0; i < c->nr_opp && i < LUT_MAX_ENTRIES; i++) {
		unsigned int sw_freq = policy->freq_table[i].frequency;
		u32 raw = readl_relaxed(c->reg_bases[REG_FREQ_LUT_TABLE] + (i * LUT_ROW_SIZE));
		unsigned int hw_freq = FIELD_GET(LUT_FREQ, raw) * 1000;
		const char *mark = (i == cur_idx) ? "*" : " ";
		const char *match = (sw_freq == hw_freq) ? "OK" : "MISMATCH";

		*off += scnprintf(buf + *off, bufsize - *off,
			"[%2d]%s %10u KHz  %10u KHz   %s\n", i, mark, sw_freq, hw_freq, match);

		if (*off >= bufsize - 128)
			return;
	}

	cpufreq_cpu_put(policy);
}

static int cpu_lut_dump_get(char *buf, const struct kernel_param *kp)
{
	void *seen_tables[8] = {0};
	int n_seen = 0;
	size_t off = 0;
	int i;

	off += scnprintf(g_dump_buf + off, DUMP_BUF_SIZE - off,
		"cpu_lut_dump: cross-check policy->freq_table (RAM) vs REG_FREQ_LUT_TABLE (MMIO)\n");

	for (i = 0; i < ARRAY_SIZE(probe_cpus); i++)
		dump_one_cpu(probe_cpus[i], g_dump_buf, &off, DUMP_BUF_SIZE, seen_tables, &n_seen);

	return scnprintf(buf, PAGE_SIZE, "%s", g_dump_buf);
}
static const struct kernel_param_ops cpu_lut_dump_ops = { .get = cpu_lut_dump_get };
module_param_cb(cpu_lut_dump, &cpu_lut_dump_ops, NULL, 0444);
MODULE_PARM_DESC(cpu_lut_dump, "READ-ONLY: dump policy->freq_table vs REG_FREQ_LUT_TABLE per domain");

/* ===== CPU STATS REFRESH — debug/manual only, NOT called from cpu_oc_apply =====
 * cpufreq_stats (drivers/cpufreq/cpufreq_stats.c) builds its
 * time_in_state bucket table ONCE, in cpufreq_stats_create_table(),
 * from a snapshot of freq_table taken at policy-online time. It never
 * re-scans freq_table afterward, and mtk-cpufreq-hw's policy is never
 * torn down by CPU hotplug (it's bound to a persistent platform
 * device, not per-CPU), so patching idx0 post-boot leaves the old
 * bucket label in place with no way to refresh it short of a reboot.
 *
 * This forces a rebuild by calling the same free+create pair the
 * cpufreq core calls internally on offline/online, done manually
 * here under policy->rwsem. Confirmed against this kernel's actual
 * drivers/cpufreq/cpufreq_stats.c (android12-5.10 branch):
 *   void cpufreq_stats_free_table(struct cpufreq_policy *policy)
 *   void cpufreq_stats_create_table(struct cpufreq_policy *policy)
 * Neither is EXPORT_SYMBOL'd, hence kallsyms resolution below.
 *
 * cpufreq_stats_create_table() seeds last_index from policy->cur:
 *   stats->last_index = freq_table_get_index(stats, policy->cur);
 * Our idx0 patch writes MMIO + policy->freq_table[0] directly,
 * bypassing the normal cpufreq_freq_transition_begin/end notifier
 * chain — so policy->cur can still hold the pre-patch value at
 * refresh time. If it doesn't match any entry in the freshly-built
 * table, last_index resolves invalid and live time only starts
 * accumulating again after the next real governor transition (no
 * crash either way — show_time_in_state() just skips the live-time
 * add when nothing matches). Syncing policy->cur to the patched
 * value here avoids that gap. */
static char cpu_stats_refresh_result[128] = "not triggered yet";

static __nocfi int do_stats_refresh(unsigned int rep_cpu, unsigned int cur_khz)
{
	struct cpufreq_policy *policy;

	if (!sym_cpufreq_stats_free_table || !sym_cpufreq_stats_create_table)
		return -ENOSYS;

	policy = cpufreq_cpu_get(rep_cpu);
	if (!policy)
		return -ENODEV;

	down_write(&policy->rwsem);
	if (cur_khz)
		policy->cur = cur_khz;   /* sync before create_table reads policy->cur */
	sym_cpufreq_stats_free_table(policy);
	sym_cpufreq_stats_create_table(policy);
	up_write(&policy->rwsem);

	cpufreq_cpu_put(policy);
	return 0;
}

static int cpu_stats_refresh_set(const char *val, const struct kernel_param *kp)
{
	unsigned int which;
	int ret;

	if (kstrtouint(val, 10, &which))
		return -EINVAL;

	/* 1 = little cluster, 2 = big cluster. One at a time, on purpose. */
	if (which != 1 && which != 2)
		return -EINVAL;

	if (atomic_read(&oc_mt6789_suspended)) {
		snprintf(cpu_stats_refresh_result, sizeof(cpu_stats_refresh_result),
			 "FAIL: device suspending, refused");
		return 0;
	}

	mutex_lock(&oc_lock);
	ret = do_stats_refresh(which == 1 ? cpu_ll_rep_cpu : cpu_b_rep_cpu,
				which == 1 ? cpu_ll_target_khz : cpu_b_target_khz);
	if (ret == -ENOSYS)
		snprintf(cpu_stats_refresh_result, sizeof(cpu_stats_refresh_result),
			 "FAIL: stats symbols not resolved");
	else if (ret == -ENODEV)
		snprintf(cpu_stats_refresh_result, sizeof(cpu_stats_refresh_result),
			 "FAIL: cpu%u not found/online", which == 1 ? cpu_ll_rep_cpu : cpu_b_rep_cpu);
	else
		snprintf(cpu_stats_refresh_result, sizeof(cpu_stats_refresh_result),
			 "OK: refreshed cluster %u — check time_in_state now", which);
	mutex_unlock(&oc_lock);
	return 0;
}
static int cpu_stats_refresh_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", cpu_stats_refresh_result);
}
static const struct kernel_param_ops cpu_stats_refresh_ops = {
	.set = cpu_stats_refresh_set,
	.get = cpu_stats_refresh_get,
};
static int cpu_stats_refresh_dummy = 0;
module_param_cb(cpu_stats_refresh, &cpu_stats_refresh_ops, &cpu_stats_refresh_dummy, 0644);
MODULE_PARM_DESC(cpu_stats_refresh, "DEBUG: write 1=little / 2=big to rebuild that cluster's time_in_state table");

/* ========== Module init/exit ========== */
static int __init oc_mt6789_init(void)
{
	unsigned long addr;
	int ret;

	pr_info("overclock_mt6789: init (GPU working_table patch + CPU cpufreq-hw LUT patch)\n");
	pr_info("oc_mt6789: CPU safety cap = stock +%d%%, %uMHz absolute ceiling (whichever is lower); raise in small steps and stress-test between each\n",
		MAX_OC_PERCENT_OVER_STOCK, MAX_OC_ABSOLUTE_KHZ / 1000);

	ret = register_pm_notifier(&oc_mt6789_pm_nb);
	if (ret)
		pr_warn("oc_mt6789: register_pm_notifier failed (%d) — suspend guard inactive\n", ret);

	g_oc_wq = alloc_workqueue("oc_mt6789_wq", WQ_HIGHPRI | WQ_UNBOUND, 1);
	if (!g_oc_wq) {
		pr_err("oc_mt6789: failed to create workqueue\n");
		unregister_pm_notifier(&oc_mt6789_pm_nb);
		return -ENOMEM;
	}
	INIT_DELAYED_WORK(&g_gpu_pll_work, gpu_pll_work_fn);

	/* apmixed ioremap is deferred to lazy_map_hw() on first GPU apply. */

	addr = resolve_ksym("gpufreq_get_working_table");
	if (addr)
		sym_gpufreq_get_working_table = (fn_get_wt_t)addr;
	else
		pr_warn("oc_mt6789: gpufreq_get_working_table not found — GPU module loaded?\n");

	addr = resolve_ksym("gpufreq_get_opp_num");
	if (addr)
		sym_gpufreq_get_opp_num = (fn_get_opp_num_t)addr;

	addr = resolve_ksym("gpufreq_get_signed_table");
	if (addr)
		sym_gpufreq_get_signed_table = (fn_get_wt_t)addr;
	else
		pr_warn("oc_mt6789: gpufreq_get_signed_table not found — signed table stays stock\n");

	addr = resolve_ksym("kallsyms_lookup_name");
	if (addr) {
		sym_kallsyms_lookup_name = (fn_kallsyms_lookup_name_t)addr;
		addr = sym_kallsyms_lookup_name("mtk_fh_set_rate");
		if (addr)
			/* addr is the variable's address — dereference for the fn ptr. */
			sym_mtk_fh_set_rate = *(fn_mtk_fh_set_rate_t *)addr;
		else
			pr_warn("oc_mt6789: mtk_fh_set_rate not found — GPU PCW change fails safe\n");

		/* Valid only once GED's own init has run — resync always
		 * rechecks *sym_ged_g_working_table for NULL. */
		addr = sym_kallsyms_lookup_name("g_working_table");
		if (addr) {
			sym_ged_g_working_table = (struct gpufreq_opp_info **)addr;
			pr_info("oc_mt6789: found GED's g_working_table via kallsyms — current_freqency will reflect OC\n");
		} else {
			pr_warn("oc_mt6789: g_working_table not found via kallsyms (CONFIG_KALLSYMS_ALL off, ged.ko not loaded yet, or gpufreq v1 build) — GPU OC still applies at hardware level, but /sys/kernel/ged/hal/current_freqency will keep showing stock freq\n");
		}

		addr = sym_kallsyms_lookup_name("cpufreq_stats_free_table");
		if (addr)
			sym_cpufreq_stats_free_table = (fn_stats_free_t)addr;
		else
			pr_warn("oc_mt6789: cpufreq_stats_free_table not found — cpu_stats_refresh unavailable\n");

		addr = sym_kallsyms_lookup_name("cpufreq_stats_create_table");
		if (addr)
			sym_cpufreq_stats_create_table = (fn_stats_create_t)addr;
		else
			pr_warn("oc_mt6789: cpufreq_stats_create_table not found — cpu_stats_refresh unavailable\n");
	} else {
		pr_warn("oc_mt6789: kallsyms_lookup_name not found — GPU PCW change fails safe, and GED sync is unavailable\n");
	}

	memset(&krp_gpufreq_commit, 0, sizeof(krp_gpufreq_commit));
	krp_gpufreq_commit.kp.symbol_name = "gpufreq_commit";
	krp_gpufreq_commit.entry_handler  = krp_gpufreq_commit_entry;
	krp_gpufreq_commit.handler        = krp_gpufreq_commit_ret;
	krp_gpufreq_commit.data_size      = sizeof(struct gpu_commit_args);
	krp_gpufreq_commit.maxactive      = 4;
	ret = register_kretprobe(&krp_gpufreq_commit);
	if (ret < 0)
		pr_warn("oc_mt6789: kretprobe gpufreq_commit failed (%d)\n", ret);

	g_guardian_thread = kthread_run(guardian_thread_fn, NULL, "oc_mt6789_guard");
	if (IS_ERR(g_guardian_thread)) {
		pr_warn("oc_mt6789: guardian thread failed to start (%ld) — CPU scaling_max_freq may drift back to stock\n",
			PTR_ERR(g_guardian_thread));
		g_guardian_thread = NULL;
	}

	pr_info("oc_mt6789: ready — sysfs at /sys/module/overclock_mt6789/parameters/\n");
	return 0;
}

static void __exit oc_mt6789_exit(void)
{
	pr_info("overclock_mt6789: unloading\n");

	unregister_pm_notifier(&oc_mt6789_pm_nb);

	if (g_guardian_thread) {
		kthread_stop(g_guardian_thread);
		g_guardian_thread = NULL;
	}

	mutex_lock(&oc_lock);
	g_gpu_oc_active = false;
	mutex_unlock(&oc_lock);

	if (g_oc_wq) {
		cancel_delayed_work_sync(&g_gpu_pll_work);
		destroy_workqueue(g_oc_wq);
		g_oc_wq = NULL;
	}

	if (krp_gpufreq_commit.kp.addr)
		unregister_kretprobe(&krp_gpufreq_commit);

	mutex_lock(&oc_lock);
	gpu_restore_working_table();
	if (g_ll_have_orig)
		patch_cluster_idx0(cpu_ll_rep_cpu, 0, &g_ll_orig_khz, &g_ll_have_orig);
	if (g_b_have_orig)
		patch_cluster_idx0(cpu_b_rep_cpu, 0, &g_b_orig_khz, &g_b_have_orig);
	mutex_unlock(&oc_lock);

	if (g_apmixed_va) {
		iounmap(g_apmixed_va);
		g_apmixed_va = NULL;
	}

	pr_info("overclock_mt6789: unloaded, stock restored where patched\n");
}

module_init(oc_mt6789_init);
module_exit(oc_mt6789_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Anomali1304");
MODULE_DESCRIPTION("GPU (working_table) + CPU (cpufreq-hw LUT) OC for MT6789 Helio G99 — POCO M5 rock");
MODULE_VERSION("1.0");
