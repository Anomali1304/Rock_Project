// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2020 MediaTek Inc.
 */

#include <linux/bitfield.h>
#include <linux/cpufreq.h>
#include <linux/energy_model.h>
#include <linux/init.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/pm_qos.h>
#include <linux/spinlock.h>
#include <linux/slab.h>

#define LUT_MAX_ENTRIES			32U
#define LUT_FREQ			GENMASK(11, 0)
#define LUT_ROW_SIZE			0x4
#define CPUFREQ_HW_STATUS		BIT(0)
#define SVS_HW_STATUS			BIT(1)
#define POLL_USEC			1000
#define TIMEOUT_USEC			300000

enum {
	REG_FREQ_LUT_TABLE,
	REG_FREQ_ENABLE,
	REG_FREQ_PERF_STATE,
	REG_FREQ_HW_STATE,
	REG_EM_POWER_TBL,
	REG_FREQ_LATENCY,

	REG_ARRAY_SIZE,
};

struct cpufreq_mtk {
	struct cpufreq_frequency_table *table;
	void __iomem *reg_bases[REG_ARRAY_SIZE];
	int nr_opp;
	cpumask_t related_cpus;
	struct cpufreq_mtk *shared;
	spinlock_t lock;
	unsigned int requested_idx[2];
	unsigned int users;
	unsigned int virtual_id;
};

static const u16 cpufreq_mtk_offsets[REG_ARRAY_SIZE] = {
	[REG_FREQ_LUT_TABLE]	= 0x0,
	[REG_FREQ_ENABLE]	= 0x84,
	[REG_FREQ_PERF_STATE]	= 0x88,
	[REG_FREQ_HW_STATE]	= 0x8c,
	[REG_EM_POWER_TBL]	= 0x90,
	[REG_FREQ_LATENCY]	= 0x114,
};

static struct cpufreq_mtk *mtk_freq_domain_map[NR_CPUS];

static int look_up_cpu(struct device *cpu_dev)
{
	int i = 0;

	for (i = 0; i < NR_CPUS; i++) {
		if (cpu_dev == get_cpu_device(i))
			return i;
	}

	return 0; /* fallback to cpu0 */
}


static int __maybe_unused
mtk_cpufreq_get_cpu_power(unsigned long *power, unsigned long *KHz,
		struct device *cpu_dev)
{
	int cpu = look_up_cpu(cpu_dev);
	struct cpufreq_mtk *c = mtk_freq_domain_map[cpu];
	struct cpufreq_mtk *shared = c->shared;
	unsigned long target = *KHz;
	int i;

	if (shared != c) {
		for (i = 0; i < shared->nr_opp; i++) {
			if (shared->table[i].frequency <= target)
				break;
		}
		if (i >= shared->nr_opp)
			i = shared->nr_opp - 1;
		*power = readl_relaxed(shared->reg_bases[REG_EM_POWER_TBL] +
			    i * LUT_ROW_SIZE) / 1000;
		return 0;
	}

	for (i = 0; i < c->nr_opp; i++) {
		if (c->table[i].frequency < *KHz)
			break;
	}
	i--;

	*KHz = c->table[i].frequency;
	*power = readl_relaxed(c->reg_bases[REG_EM_POWER_TBL] +
			    i * LUT_ROW_SIZE) / 1000;

	return 0;
}
static unsigned int mtk_virtual_a55_to_hw_index(
		struct cpufreq_mtk *shared, unsigned int target_freq)
{
	unsigned int i;

	for (i = 0; i < shared->nr_opp; i++) {
		if (shared->table[i].frequency <= target_freq)
			return i;
	}

	return shared->nr_opp ? shared->nr_opp - 1 : 0;
}

static int mtk_cpufreq_hw_target_index(struct cpufreq_policy *policy,
				       unsigned int index)
{
	struct cpufreq_mtk *c = policy->driver_data;
	struct cpufreq_mtk *shared = c->shared;
	unsigned long flags;
	unsigned int hw_index;

	if (shared != c) {
		hw_index = mtk_virtual_a55_to_hw_index(shared,
			policy->freq_table[index].frequency);

		spin_lock_irqsave(&shared->lock, flags);
		shared->requested_idx[c->virtual_id] = hw_index;
		/* LUT index 0 is the highest frequency. Choose the lower index so
		 * the shared A55 hardware satisfies the highest request. */
		hw_index = min(shared->requested_idx[0],
				       shared->requested_idx[1]);
		writel_relaxed(hw_index,
				shared->reg_bases[REG_FREQ_PERF_STATE]);
		spin_unlock_irqrestore(&shared->lock, flags);
		return 0;
	}

	spin_lock_irqsave(&c->lock, flags);
	c->requested_idx[0] = index;
	if (c->users > 1)
		hw_index = min(c->requested_idx[0], c->requested_idx[1]);
	else
		hw_index = index;
	writel_relaxed(hw_index, c->reg_bases[REG_FREQ_PERF_STATE]);
	spin_unlock_irqrestore(&c->lock, flags);

	return 0;
}

static unsigned int mtk_cpufreq_hw_get(unsigned int cpu)
{
	struct cpufreq_mtk *c;
	struct cpufreq_mtk *shared;
	unsigned int hw_index;
	unsigned int hw_freq;
	unsigned int i;

	c = mtk_freq_domain_map[cpu];
	if (!c)
		return 0;

	shared = c->shared;
	hw_index = readl_relaxed(shared->reg_bases[REG_FREQ_PERF_STATE]);
	hw_index = min(hw_index, (unsigned int)(shared->nr_opp ? shared->nr_opp - 1 : 0));
	hw_freq = shared->table[hw_index].frequency;

	if (c == shared)
		return hw_freq;

	for (i = 0; i < c->nr_opp; i++) {
		if (c->table[i].frequency <= hw_freq)
			return c->table[i].frequency;
	}

	return c->table[c->nr_opp ? c->nr_opp - 1 : 0].frequency;
}

static unsigned int mtk_cpufreq_hw_fast_switch(struct cpufreq_policy *policy,
					       unsigned int target_freq)
{
	struct cpufreq_mtk *c = policy->driver_data;
	struct cpufreq_mtk *shared = c->shared;
	unsigned int index;
	unsigned int hw_index;
	unsigned long flags;

	if (policy->cached_target_freq == target_freq)
		index = policy->cached_resolved_idx;
	else
		index = cpufreq_table_find_index_dl(policy, target_freq);

	if (shared != c) {
		hw_index = mtk_virtual_a55_to_hw_index(shared,
			policy->freq_table[index].frequency);
		spin_lock_irqsave(&shared->lock, flags);
		shared->requested_idx[c->virtual_id] = hw_index;
		/* LUT index 0 is the highest frequency. Choose the lower index so
		 * the shared A55 hardware satisfies the highest request. */
		hw_index = min(shared->requested_idx[0],
				       shared->requested_idx[1]);
		writel_relaxed(hw_index,
				shared->reg_bases[REG_FREQ_PERF_STATE]);
		spin_unlock_irqrestore(&shared->lock, flags);
		return policy->freq_table[index].frequency;
	}

	spin_lock_irqsave(&c->lock, flags);
	c->requested_idx[0] = index;
	if (c->users > 1)
		hw_index = min(c->requested_idx[0], c->requested_idx[1]);
	else
		hw_index = index;
	writel_relaxed(hw_index, c->reg_bases[REG_FREQ_PERF_STATE]);
	spin_unlock_irqrestore(&c->lock, flags);

	return c->table[hw_index].frequency;
}

static int mtk_cpufreq_hw_cpu_init(struct cpufreq_policy *policy)
{
	struct cpufreq_mtk *c;
	struct device *cpu_dev;
	struct em_data_callback em_cb = EM_DATA_CB(mtk_cpufreq_get_cpu_power);
	struct pm_qos_request *qos_request;
	int sig, pwr_hw = CPUFREQ_HW_STATUS | SVS_HW_STATUS;
	unsigned int latency;

	cpu_dev = get_cpu_device(policy->cpu);
	if (!cpu_dev) {
		pr_info("failed to get cpu%d device\n", policy->cpu);
		return -ENODEV;
	}

	c = mtk_freq_domain_map[policy->cpu];
	if (!c) {
		pr_info("No scaling support for CPU%d\n", policy->cpu);
		return -ENODEV;
	}

	cpumask_copy(policy->cpus, &c->related_cpus);

	policy->freq_table = c->table;
	policy->driver_data = c;

	if (c->shared == c) {
		unsigned long flags;

		spin_lock_irqsave(&c->lock, flags);
		if (!c->users) {
			unsigned int current_idx = readl_relaxed(
				c->reg_bases[REG_FREQ_PERF_STATE]);

			current_idx = min(current_idx, LUT_MAX_ENTRIES - 1);
			c->requested_idx[0] = current_idx;
			c->requested_idx[1] = current_idx;
		}
		c->users++;
		spin_unlock_irqrestore(&c->lock, flags);
	} else {
		unsigned long flags;

		spin_lock_irqsave(&c->shared->lock, flags);
		c->shared->users++;
		spin_unlock_irqrestore(&c->shared->lock, flags);
	}

	latency = readl_relaxed(c->reg_bases[REG_FREQ_LATENCY]);
	if (!latency)
		latency = CPUFREQ_ETERNAL;

	/* us convert to ns */
	policy->cpuinfo.transition_latency = latency * 1000;

	policy->fast_switch_possible = true;

	qos_request = kzalloc(sizeof(*qos_request), GFP_KERNEL);
	if (!qos_request)
		return -ENOMEM;

	/* Let CPUs leave idle-off state for SVS CPU initializing */
	cpu_latency_qos_add_request(qos_request, PM_QOS_DEFAULT_VALUE);

	/* HW should be in enabled state to proceed now */
	writel_relaxed(0x1, c->reg_bases[REG_FREQ_ENABLE]);

	if (readl_poll_timeout(c->reg_bases[REG_FREQ_HW_STATE], sig,
			       (sig & pwr_hw) == pwr_hw, POLL_USEC,
			       TIMEOUT_USEC)) {
		if (!(sig & CPUFREQ_HW_STATUS)) {
			pr_info("cpufreq hardware of CPU%d is not enabled\n",
				policy->cpu);
			cpu_latency_qos_remove_request(qos_request);
			kfree(qos_request);
			return -ENODEV;
		}

		pr_info("SVS of CPU%d is not enabled\n", policy->cpu);
	}

	em_dev_register_perf_domain(cpu_dev, c->nr_opp, &em_cb, policy->cpus,
			true);
	cpu_latency_qos_remove_request(qos_request);
	kfree(qos_request);

	return 0;
}

static int mtk_cpufreq_hw_cpu_exit(struct cpufreq_policy *policy)
{
	struct cpufreq_mtk *c;

	c = mtk_freq_domain_map[policy->cpu];
	if (!c) {
		pr_info("No scaling support for CPU%d\n", policy->cpu);
		return -ENODEV;
	}

	if (c->shared == c) {
		unsigned long flags;

		spin_lock_irqsave(&c->lock, flags);
		if (c->users)
			c->users--;
		c->requested_idx[0] = c->nr_opp ? c->nr_opp - 1 : 0;
		if (!c->users) {
			writel_relaxed(0x0, c->reg_bases[REG_FREQ_ENABLE]);
		} else {
			writel_relaxed(min(c->requested_idx[0], c->requested_idx[1]),
				c->reg_bases[REG_FREQ_PERF_STATE]);
		}
		spin_unlock_irqrestore(&c->lock, flags);
	} else {
		unsigned long flags;
		struct cpufreq_mtk *shared = c->shared;

		spin_lock_irqsave(&shared->lock, flags);
		shared->requested_idx[c->virtual_id] = shared->nr_opp ? shared->nr_opp - 1 : 0;
		if (shared->users)
			shared->users--;
		if (!shared->users) {
			writel_relaxed(0x0, shared->reg_bases[REG_FREQ_ENABLE]);
		} else {
			writel_relaxed(min(shared->requested_idx[0],
					shared->requested_idx[1]),
				shared->reg_bases[REG_FREQ_PERF_STATE]);
		}
		spin_unlock_irqrestore(&shared->lock, flags);
	}

	return 0;
}

static struct cpufreq_driver cpufreq_mtk_hw_driver = {
	.flags		= CPUFREQ_STICKY | CPUFREQ_NEED_INITIAL_FREQ_CHECK |
			  CPUFREQ_HAVE_GOVERNOR_PER_POLICY |
			  CPUFREQ_IS_COOLING_DEV,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= mtk_cpufreq_hw_target_index,
	.get		= mtk_cpufreq_hw_get,
	.init		= mtk_cpufreq_hw_cpu_init,
	.exit		= mtk_cpufreq_hw_cpu_exit,
	.fast_switch	= mtk_cpufreq_hw_fast_switch,
	.name		= "mtk-cpufreq-hw",
	.attr		= cpufreq_generic_attr,
};

static int mtk_cpu_create_freq_table(struct platform_device *pdev,
				     struct cpufreq_mtk *c)
{
	struct device *dev = &pdev->dev;
	void __iomem *base_table;
	u32 data, i, freq, prev_freq = 0;

	c->table = devm_kcalloc(dev, LUT_MAX_ENTRIES + 1,
				sizeof(*c->table), GFP_KERNEL);
	if (!c->table)
		return -ENOMEM;

	base_table = c->reg_bases[REG_FREQ_LUT_TABLE];

	for (i = 0; i < LUT_MAX_ENTRIES; i++) {
		data = readl_relaxed(base_table + (i * LUT_ROW_SIZE));
		freq = FIELD_GET(LUT_FREQ, data) * 1000;

		if (freq == prev_freq)
			break;

		c->table[i].frequency = freq;

		dev_dbg(dev, "index=%d freq=%d\n",
			i, c->table[i].frequency);

		prev_freq = freq;
	}

	c->table[i].frequency = CPUFREQ_TABLE_END;
	c->nr_opp = i;

	return 0;
}

static int mtk_get_related_cpus(int index, struct cpufreq_mtk *c)
{
	struct device_node *cpu_np;
	struct of_phandle_args args;
	int cpu, ret;

	for_each_possible_cpu(cpu) {
		cpu_np = of_cpu_device_node_get(cpu);
		if (!cpu_np)
			continue;

		ret = of_parse_phandle_with_args(cpu_np, "performance-domains",
						 "#performance-domain-cells", 0,
						 &args);
		of_node_put(cpu_np);
		if (ret < 0)
			continue;

		if (index == args.args[0]) {
			if (index == 0 && cpu >= 3)
				continue;
			cpumask_set_cpu(cpu, &c->related_cpus);
			mtk_freq_domain_map[cpu] = c;
		}
	}

	return 0;
}

static int mtk_cpu_resources_init(struct platform_device *pdev,
				  unsigned int cpu, int index,
				  const u16 *offsets)
{
	struct cpufreq_mtk *c;
	struct device *dev = &pdev->dev;
	int ret, i;
	void __iomem *base;

	if (mtk_freq_domain_map[cpu])
		return 0;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	if (index == 0 && cpu >= 3 && cpu <= 5 &&
	    mtk_freq_domain_map[0]) {
		struct cpufreq_mtk *shared = mtk_freq_domain_map[0];

		c->shared = shared;
		c->virtual_id = 1;
		c->table = shared->table;
		c->nr_opp = shared->nr_opp;
		for (i = 0; i < REG_ARRAY_SIZE; i++)
			c->reg_bases[i] = shared->reg_bases[i];

		cpumask_clear(&c->related_cpus);
		for (i = 3; i <= 5; i++) {
			cpumask_set_cpu(i, &c->related_cpus);
			mtk_freq_domain_map[i] = c;
		}
		return 0;
	}

	c->shared = c;
	c->virtual_id = 0;
	c->requested_idx[0] = 0;
	c->requested_idx[1] = 0;
	spin_lock_init(&c->lock);

	base = devm_platform_ioremap_resource(pdev, index);
	if (IS_ERR(base))
		return PTR_ERR(base);

	for (i = REG_FREQ_LUT_TABLE; i < REG_ARRAY_SIZE; i++)
		c->reg_bases[i] = base + offsets[i];

	ret = mtk_get_related_cpus(index, c);
	if (ret) {
		dev_info(dev, "Domain-%d failed to get related CPUs\n", index);
		return ret;
	}

	ret = mtk_cpu_create_freq_table(pdev, c);
	if (ret) {
		dev_info(dev, "Domain-%d failed to create freq table\n", index);
		return ret;
	}

	if (c->nr_opp) {
		unsigned int current_idx = readl_relaxed(
			c->reg_bases[REG_FREQ_PERF_STATE]);

		current_idx = min(current_idx, (unsigned int)c->nr_opp - 1);
		c->requested_idx[0] = current_idx;
		c->requested_idx[1] = current_idx;
	}

	return 0;
}

static int mtk_finalize_virtual_a55_policy(struct platform_device *pdev)
{
	struct cpufreq_mtk *virt = mtk_freq_domain_map[3];
	struct cpufreq_mtk *a55 = mtk_freq_domain_map[0];

	if (!virt || !virt->shared || virt->shared != a55)
		return -ENODEV;
	if (!a55 || !a55->table || !a55->nr_opp)
		return -ENODEV;

	/* CPU3-5 are a logical policy only. Keep the real A55 LUT so the
	 * policy never advertises frequencies the A55 hardware cannot run. */
	virt->table = a55->table;
	virt->nr_opp = a55->nr_opp;
	return 0;
}

static int mtk_cpufreq_hw_driver_probe(struct platform_device *pdev)
{
	struct device_node *cpu_np;
	struct of_phandle_args args;
	const u16 *offsets;
	unsigned int cpu;
	int ret;

	offsets = of_device_get_match_data(&pdev->dev);
	if (!offsets)
		return -EINVAL;

	for_each_possible_cpu(cpu) {
		cpu_np = of_cpu_device_node_get(cpu);
		if (!cpu_np) {
			dev_info(&pdev->dev, "Failed to get cpu %d device\n",
				cpu);
			return -ENODEV;
		}

		ret = of_parse_phandle_with_args(cpu_np, "performance-domains",
						 "#performance-domain-cells", 0,
						 &args);
		if (ret < 0)
			return ret;

		/* Get the physical CPUFreq domains; CPU3-5 are split logically below. */
		ret = mtk_cpu_resources_init(pdev, cpu, args.args[0], offsets);
		if (ret) {
			dev_info(&pdev->dev, "CPUFreq resource init failed\n");
			return ret;
		}
	}

	ret = mtk_finalize_virtual_a55_policy(pdev);
	if (ret) {
		dev_info(&pdev->dev, "Virtual A55 policy setup failed: %d\n", ret);
		return ret;
	}

	ret = cpufreq_register_driver(&cpufreq_mtk_hw_driver);
	if (ret) {
		dev_info(&pdev->dev, "CPUFreq HW driver failed to register\n");
		return ret;
	}

	return 0;
}

static int mtk_cpufreq_hw_driver_remove(struct platform_device *pdev)
{
	return cpufreq_unregister_driver(&cpufreq_mtk_hw_driver);
}

static const struct of_device_id mtk_cpufreq_hw_match[] = {
	{ .compatible = "mediatek,cpufreq-hw", .data = &cpufreq_mtk_offsets },
	{}
};

static struct platform_driver mtk_cpufreq_hw_driver = {
	.probe = mtk_cpufreq_hw_driver_probe,
	.remove = mtk_cpufreq_hw_driver_remove,
	.driver = {
		.name = "mtk-cpufreq-hw",
		.of_match_table = mtk_cpufreq_hw_match,
	},
};
module_platform_driver(mtk_cpufreq_hw_driver);

MODULE_DESCRIPTION("Mediatek cpufreq-hw driver");
MODULE_LICENSE("GPL v2");