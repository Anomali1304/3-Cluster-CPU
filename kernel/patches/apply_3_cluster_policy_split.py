#!/usr/bin/env python3
from pathlib import Path
import sys

path = Path(next((a for a in sys.argv[1:] if not a.startswith('--')),
                 'drivers/cpufreq/mediatek-cpufreq-hw.c'))
s = path.read_text()

def once(old, new, label):
    global s
    n = s.count(old)
    if n != 1:
        raise SystemExit(f'3-cluster: {label}: expected 1 match, got {n}')
    s = s.replace(old, new)

once('''#include <linux/pm_qos.h>
#include <linux/slab.h>''', '''#include <linux/pm_qos.h>
#include <linux/spinlock.h>
#include <linux/slab.h>''', 'headers')

once('''struct cpufreq_mtk {
	struct cpufreq_frequency_table *table;
	void __iomem *reg_bases[REG_ARRAY_SIZE];
	int nr_opp;
	cpumask_t related_cpus;
};''', '''struct cpufreq_mtk {
	struct cpufreq_frequency_table *table;
	void __iomem *reg_bases[REG_ARRAY_SIZE];
	int nr_opp;
	cpumask_t related_cpus;
	struct cpufreq_mtk *shared;
	spinlock_t lock;
	unsigned int requested_idx[2];
	unsigned int active_users;
	bool virtual_policy;
};''', 'struct')

once('''static int mtk_cpufreq_hw_target_index(struct cpufreq_policy *policy,
				       unsigned int index)
{
	struct cpufreq_mtk *c = policy->driver_data;

	writel_relaxed(index, c->reg_bases[REG_FREQ_PERF_STATE]);

	return 0;
}''', '''static int mtk_cpufreq_hw_target_index(struct cpufreq_policy *policy,
				       unsigned int index)
{
	struct cpufreq_mtk *c = policy->driver_data;
	struct cpufreq_mtk *hw = c->shared;
	unsigned long flags;
	unsigned int hw_index;

	if (!c->nr_opp || index >= c->nr_opp)
		return -EINVAL;

	hw_index = index;

	spin_lock_irqsave(&hw->lock, flags);
	hw->requested_idx[c->virtual_policy ? 1 : 0] = hw_index;
	hw_index = min(hw->requested_idx[0], hw->requested_idx[1]);
	writel_relaxed(hw_index, hw->reg_bases[REG_FREQ_PERF_STATE]);
	spin_unlock_irqrestore(&hw->lock, flags);

	return 0;
}''', 'target')

once('''static unsigned int mtk_cpufreq_hw_get(unsigned int cpu)
{
	struct cpufreq_mtk *c;
	unsigned int index;

	c = mtk_freq_domain_map[cpu];

	index = readl_relaxed(c->reg_bases[REG_FREQ_PERF_STATE]);
	index = min(index, LUT_MAX_ENTRIES - 1);

	return c->table[index].frequency;
}''', '''static unsigned int mtk_cpufreq_hw_get(unsigned int cpu)
{
	struct cpufreq_mtk *c = mtk_freq_domain_map[cpu];
	struct cpufreq_mtk *hw;
	unsigned int index;

	if (!c)
		return 0;

	hw = c->shared;
	index = readl_relaxed(hw->reg_bases[REG_FREQ_PERF_STATE]);
	index = min(index, (unsigned int)(hw->nr_opp - 1));

	return hw->table[index].frequency;
}''', 'get')

once('''static unsigned int mtk_cpufreq_hw_fast_switch(struct cpufreq_policy *policy,
					       unsigned int target_freq)
{
	struct cpufreq_mtk *c = policy->driver_data;
	unsigned int index;

	if (policy->cached_target_freq == target_freq)
		index = policy->cached_resolved_idx;
	else
		index = cpufreq_table_find_index_dl(policy, target_freq);

	writel_relaxed(index, c->reg_bases[REG_FREQ_PERF_STATE]);

	return policy->freq_table[index].frequency;
}''', '''static unsigned int mtk_cpufreq_hw_fast_switch(struct cpufreq_policy *policy,
					       unsigned int target_freq)
{
	struct cpufreq_mtk *c = policy->driver_data;
	struct cpufreq_mtk *hw = c->shared;
	unsigned int index;
	unsigned int hw_index;
	unsigned long flags;

	if (policy->cached_target_freq == target_freq)
		index = policy->cached_resolved_idx;
	else
		index = cpufreq_table_find_index_dl(policy, target_freq);

	if (index >= c->nr_opp)
		index = c->nr_opp - 1;

	hw_index = index;

	spin_lock_irqsave(&hw->lock, flags);
	hw->requested_idx[c->virtual_policy ? 1 : 0] = hw_index;
	hw_index = min(hw->requested_idx[0], hw->requested_idx[1]);
	writel_relaxed(hw_index, hw->reg_bases[REG_FREQ_PERF_STATE]);
	spin_unlock_irqrestore(&hw->lock, flags);

	return policy->freq_table[index].frequency;
}''', 'fast switch')

once('''static int mtk_cpu_resources_init(struct platform_device *pdev,
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

	base = devm_platform_ioremap_resource(pdev, index);
	if (IS_ERR(base))
		return PTR_ERR(base);

	for (i = REG_FREQ_LUT_TABLE; i < REG_ARRAY_SIZE; i++)
		c->reg_bases[i] = base + offsets[i];

	ret = mtk_get_related_cpus(index, c);
	if (ret) {
		dev_info(dev, "Domain-%d failed to get related CPUs\\n", index);
		return ret;
	}

	ret = mtk_cpu_create_freq_table(pdev, c);
	if (ret) {
		dev_info(dev, "Domain-%d failed to create freq table\\n", index);
		return ret;
	}

	return 0;
}''', '''static int mtk_cpu_resources_init(struct platform_device *pdev,
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

	base = devm_platform_ioremap_resource(pdev, index);
	if (IS_ERR(base))
		return PTR_ERR(base);

	for (i = REG_FREQ_LUT_TABLE; i < REG_ARRAY_SIZE; i++)
		c->reg_bases[i] = base + offsets[i];

	c->shared = c;
	c->active_users = 0;
	c->virtual_policy = false;
	spin_lock_init(&c->lock);

	ret = mtk_get_related_cpus(index, c);
	if (ret) {
		dev_info(dev, "Domain-%d failed to get related CPUs\\n", index);
		return ret;
	}

	ret = mtk_cpu_create_freq_table(pdev, c);
	if (ret) {
		dev_info(dev, "Domain-%d failed to create freq table\\n", index);
		return ret;
	}

	return 0;
}''', 'resources')

once('''static int mtk_cpufreq_hw_driver_probe(struct platform_device *pdev)''', '''static int mtk_create_virtual_policy(struct platform_device *pdev,
				      unsigned int cpu,
				      struct cpufreq_mtk *shared)
{
	struct cpufreq_mtk *c;
	struct device *dev = &pdev->dev;

	if (!shared || cpu >= NR_CPUS)
		return -EINVAL;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	c->table = shared->table;
	c->nr_opp = shared->nr_opp;
	c->shared = shared;
	c->virtual_policy = true;
	c->reg_bases[REG_FREQ_LUT_TABLE] = shared->reg_bases[REG_FREQ_LUT_TABLE];
	c->reg_bases[REG_FREQ_ENABLE] = shared->reg_bases[REG_FREQ_ENABLE];
	c->reg_bases[REG_FREQ_PERF_STATE] = shared->reg_bases[REG_FREQ_PERF_STATE];
	c->reg_bases[REG_FREQ_HW_STATE] = shared->reg_bases[REG_FREQ_HW_STATE];
	c->reg_bases[REG_EM_POWER_TBL] = shared->reg_bases[REG_EM_POWER_TBL];
	c->reg_bases[REG_FREQ_LATENCY] = shared->reg_bases[REG_FREQ_LATENCY];

	cpumask_clear(&c->related_cpus);
	cpumask_set_cpu(cpu, &c->related_cpus);
	mtk_freq_domain_map[cpu] = c;

	return 0;
}

static int mtk_cpufreq_hw_driver_probe(struct platform_device *pdev)''', 'virtual helper')

once('''static int mtk_cpufreq_hw_cpu_init(struct cpufreq_policy *policy)
{
	struct cpufreq_mtk *c;
	struct device *cpu_dev;
	struct em_data_callback em_cb = EM_DATA_CB(mtk_cpufreq_get_cpu_power);
	struct pm_qos_request *qos_request;
	int sig, pwr_hw = CPUFREQ_HW_STATUS | SVS_HW_STATUS;
	unsigned int latency;''', '''static int mtk_cpufreq_hw_cpu_init(struct cpufreq_policy *policy)
{
	struct cpufreq_mtk *c;
	struct cpufreq_mtk *hw;
	struct device *cpu_dev;
	struct em_data_callback em_cb = EM_DATA_CB(mtk_cpufreq_get_cpu_power);
	struct pm_qos_request *qos_request;
	int sig, pwr_hw = CPUFREQ_HW_STATUS | SVS_HW_STATUS;
	unsigned int latency;
	unsigned int current_idx;
	unsigned long flags;''', 'init locals')

once('''	cpumask_copy(policy->cpus, &c->related_cpus);

	policy->freq_table = c->table;''', '''	cpumask_copy(policy->cpus, &c->related_cpus);
	if (policy->cpu == 6 && !c->virtual_policy) {
		cpumask_clear(policy->cpus);
		cpumask_set_cpu(6, policy->cpus);
	}

	policy->freq_table = c->table;''', 'policy masks')

once('''	policy->freq_table = c->table;
	policy->driver_data = c;

	latency =''', '''	policy->freq_table = c->table;
	policy->driver_data = c;
	hw = c->shared;

	spin_lock_irqsave(&hw->lock, flags);
	if (!hw->active_users) {
		current_idx = readl_relaxed(hw->reg_bases[REG_FREQ_PERF_STATE]);
		current_idx = min(current_idx, (unsigned int)(hw->nr_opp - 1));
		hw->requested_idx[0] = current_idx;
		hw->requested_idx[1] = current_idx;
	}
	hw->active_users++;
	spin_unlock_irqrestore(&hw->lock, flags);

	latency =''', 'shared init')

once('''	/* HW should be in enabled state to proceed now */
	writel_relaxed(0x1, c->reg_bases[REG_FREQ_ENABLE]);

	if (readl_poll_timeout(c->reg_bases[REG_FREQ_HW_STATE], sig,''', '''	/* HW should be in enabled state to proceed now */
	writel_relaxed(0x1, hw->reg_bases[REG_FREQ_ENABLE]);

	if (readl_poll_timeout(hw->reg_bases[REG_FREQ_HW_STATE], sig,''', 'shared enable')

once('''		if (!(sig & CPUFREQ_HW_STATUS)) {
			pr_info("cpufreq hardware of CPU%d is not enabled\\n",
				policy->cpu);
			cpu_latency_qos_remove_request(qos_request);
			kfree(qos_request);
			return -ENODEV;
		}''', '''		if (!(sig & CPUFREQ_HW_STATUS)) {
			pr_info("cpufreq hardware of CPU%d is not enabled\\n",
				policy->cpu);
			cpu_latency_qos_remove_request(qos_request);
			kfree(qos_request);
			spin_lock_irqsave(&hw->lock, flags);
			if (hw->active_users)
				hw->active_users--;
			spin_unlock_irqrestore(&hw->lock, flags);
			return -ENODEV;
		}''', 'init rollback')

once('''static int mtk_cpufreq_hw_cpu_exit(struct cpufreq_policy *policy)
{
	struct cpufreq_mtk *c;

	c = mtk_freq_domain_map[policy->cpu];
	if (!c) {
		pr_info("No scaling support for CPU%d\\n", policy->cpu);
		return -ENODEV;
	}

	/* HW should be in paused state now */
	writel_relaxed(0x0, c->reg_bases[REG_FREQ_ENABLE]);

	return 0;
}''', '''static int mtk_cpufreq_hw_cpu_exit(struct cpufreq_policy *policy)
{
	struct cpufreq_mtk *c;
	struct cpufreq_mtk *hw;
	unsigned long flags;

	c = mtk_freq_domain_map[policy->cpu];
	if (!c) {
		pr_info("No scaling support for CPU%d\\n", policy->cpu);
		return -ENODEV;
	}

	hw = c->shared;

	spin_lock_irqsave(&hw->lock, flags);
	if (hw->active_users)
		hw->active_users--;
	hw->requested_idx[c->virtual_policy ? 1 : 0] = UINT_MAX;

	if (!hw->active_users)
		writel_relaxed(0x0, hw->reg_bases[REG_FREQ_ENABLE]);
	else
		writel_relaxed(min(hw->requested_idx[0], hw->requested_idx[1]),
			       hw->reg_bases[REG_FREQ_PERF_STATE]);

	spin_unlock_irqrestore(&hw->lock, flags);

	return 0;
}''', 'exit')

once('''	for_each_possible_cpu(cpu) {
		cpu_np = of_cpu_device_node_get(cpu);
		if (!cpu_np) {
			dev_info(&pdev->dev, "Failed to get cpu %d device\\n",
				cpu);
			return -ENODEV;
		}

		ret = of_parse_phandle_with_args(cpu_np, "performance-domains",
						 "#performance-domain-cells", 0,
						 &args);
		if (ret < 0)
			return ret;

		/* Get the bases of cpufreq for domains */
		ret = mtk_cpu_resources_init(pdev, cpu, args.args[0], offsets);
		if (ret) {
			dev_info(&pdev->dev, "CPUFreq resource init failed\\n");
			return ret;
		}
	}''', '''	for_each_possible_cpu(cpu) {
		cpu_np = of_cpu_device_node_get(cpu);
		if (!cpu_np) {
			dev_info(&pdev->dev, "Failed to get cpu %d device\\n", cpu);
			return -ENODEV;
		}

		ret = of_parse_phandle_with_args(cpu_np, "performance-domains",
						 "#performance-domain-cells", 0, &args);
		if (ret < 0)
			return ret;

		ret = mtk_cpu_resources_init(pdev, cpu, args.args[0], offsets);
		if (ret) {
			dev_info(&pdev->dev, "CPUFreq resource init failed\\n");
			return ret;
		}
	}

	if (!mtk_freq_domain_map[0] || !mtk_freq_domain_map[6])
		return -ENODEV;

	ret = mtk_create_virtual_policy(pdev, 7, mtk_freq_domain_map[6]);
	if (ret)
		return ret;''', 'probe')

path.write_text(s)
print('3-cluster: patched 6+1+1 source')
