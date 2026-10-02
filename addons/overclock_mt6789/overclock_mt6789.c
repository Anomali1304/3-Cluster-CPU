// SPDX-License-Identifier: GPL-2.0

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
#include <linux/spinlock.h>

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

enum gpufreq_posdiv {
	POSDIV_POWER_1  = 0,
	POSDIV_POWER_2  = 1,
	POSDIV_POWER_4  = 2,
	POSDIV_POWER_8  = 3,
	POSDIV_POWER_16 = 4,
};
enum gpufreq_target { TARGET_STACK = 0, TARGET_GPU = 1, TARGET_DEFAULT = 2 };

struct gpufreq_opp_info {
	unsigned int        freq;
	unsigned int        volt;
	unsigned int        vsram;
	enum gpufreq_posdiv posdiv;
	unsigned int        vaging;
	unsigned int        power;
};

typedef const struct gpufreq_opp_info *(*fn_get_wt_t)(enum gpufreq_target);
typedef int (*fn_get_opp_num_t)(enum gpufreq_target);

static fn_get_wt_t      sym_gpufreq_get_working_table;
static fn_get_opp_num_t sym_gpufreq_get_opp_num;

static fn_get_wt_t      sym_gpufreq_get_signed_table;

static struct gpufreq_opp_info **sym_ged_g_working_table;

typedef unsigned long (*fn_kallsyms_lookup_name_t)(const char *name);
static fn_kallsyms_lookup_name_t sym_kallsyms_lookup_name;

typedef bool (*fn_mtk_fh_set_rate_t)(const char *name, unsigned long dds, int postdiv);
static fn_mtk_fh_set_rate_t sym_mtk_fh_set_rate;


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

#define POSDIV_2_MAX_FREQ  1750000U
#define POSDIV_4_MIN_FREQ  375000U
#define VGPU_MAX_VOLT      100000U
#define VGPU_MIN_VOLT      50000U

#define GPU_OPP_MAX 72

static unsigned int gpu_target_freq  = 0;
static unsigned int gpu_target_volt  = 0;
static unsigned int gpu_target_vsram = 0;
static int          gpu_oc_apply     = 0;
static char gpu_oc_result[128] = "not applied";
static char gpu_opp_dump[4096] = "";

module_param(gpu_target_freq,  uint, 0644);
MODULE_PARM_DESC(gpu_target_freq,  "GPU target freq KHz (0=off, max 1750000)");
module_param(gpu_target_volt,  uint, 0644);
MODULE_PARM_DESC(gpu_target_volt,  "GPU target volt mV*100 (e.g. 90000=900mV)");
module_param(gpu_target_vsram, uint, 0644);
MODULE_PARM_DESC(gpu_target_vsram, "GPU vsram mV*100 (0=same as volt)");
module_param_string(gpu_oc_result, gpu_oc_result, sizeof(gpu_oc_result), 0444);
MODULE_PARM_DESC(gpu_oc_result, "GPU OC status (read-only)");

static DEFINE_MUTEX(oc_lock);

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

		if (!sym_mtk_fh_set_rate(pll_name, pcw, target_posdiv))
			return false;
		pll = (readl(reg) & 0xF8FFFFFFu) | ((unsigned)target_posdiv << POSDIV_SHIFT);
		writel(pll, reg);
		udelay(20);
	} else {

		pll = (readl(reg) & 0xF8FFFFFFu) | ((unsigned)target_posdiv << POSDIV_SHIFT);
		writel(pll, reg);
		udelay(20);
		if (!sym_mtk_fh_set_rate(pll_name, pcw, target_posdiv))
			return false;
	}
	return true;
}

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

static __nocfi void ged_snapshot_resync(int idx, const struct gpufreq_opp_info *val)
{
	if (!sym_ged_g_working_table || !*sym_ged_g_working_table)
		return;

	(*sym_ged_g_working_table)[idx] = *val;
}

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

static struct kretprobe krp_gpufreq_commit;

struct gpu_commit_args {
	enum gpufreq_target target;
	int                 oppidx;
};

static __nocfi int krp_gpufreq_commit_entry(struct kretprobe_instance *ri,
					    struct pt_regs *regs)
{
	struct gpu_commit_args *a = (struct gpu_commit_args *)ri->data;

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
	if (gpu_target_vsram < VGPU_MIN_VOLT || gpu_target_vsram > VGPU_MAX_VOLT) {
		snprintf(gpu_oc_result, sizeof(gpu_oc_result),
			 "FAIL: vsram %u out of [%u, %u] mV*100",
			 gpu_target_vsram, VGPU_MIN_VOLT, VGPU_MAX_VOLT);
		goto out;
	}

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

#define LUT_MAX_ENTRIES   32U
#define LUT_FREQ          GENMASK(11, 0)
#define LUT_VOLT          GENMASK(28, 12)
#define LUT_FLAG_A        BIT(29)
#define LUT_FLAG_B        BIT(30)
#define LUT_ROW_SIZE      0x4

/*
 * CPU voltage field implementation imported from Overclock_MT6789 patch-2.
 * The field was correlated against /proc/eem_lite/eem_cur_volt; it is not
 * treated as an official MediaTek register definition. The write preserves
 * all other LUT bits and only changes bits[28:12].
 */
/*
 * Raw LUT voltage unit is 10 uV (625 raw = one 6.25 mV EEM step).
 *
 * cpu_volt_abs_max_raw (runtime tunable) is the ceiling for the final idx0
 * LUT voltage. It can never exceed CPU_VOLT_ABS_HARD_MAX_RAW, which is a
 * compile-time limit that no module parameter can lift.
 */
#define CPU_VOLT_ABS_DEFAULT_RAW  112000U  /* 1.12 V */
#define CPU_VOLT_ABS_HARD_MAX_RAW 115000U  /* 1.15 V */
#define CPU_VOLT_DELTA_DEFAULT_RAW 20000U  /* +200 mV = 32 EEM steps */
#define EEM_VOLT_STEP         625U

enum {
	REG_FREQ_LUT_TABLE,
	REG_FREQ_ENABLE,
	REG_FREQ_PERF_STATE,
	REG_FREQ_HW_STATE,
	REG_EM_POWER_TBL,
	REG_FREQ_LATENCY,
	REG_ARRAY_SIZE,
};

/*
 * CPU control layout for the main 6+1+1 cpufreq-hw patch:
 *   physical A55 domain: CPU0-5
 *   physical A76 domain: CPU6-7
 *   logical policies:    CPU0-5, CPU6, CPU7
 *
 * CPU6 and CPU7 are separate Linux policies backed by the same A76
 * hardware domain. CPU0-5 remain one physical A55 policy.
 *
 * IMPORTANT: this mirror intentionally matches struct cpufreq_mtk in
 * the patched driver byte-for-byte through virtual_policy.
 */
struct cpufreq_mtk_mirror {
	struct cpufreq_frequency_table *table;
	void __iomem *reg_bases[REG_ARRAY_SIZE];
	int nr_opp;
	cpumask_t related_cpus;
	struct cpufreq_mtk_mirror *shared;
	spinlock_t lock;
	unsigned int requested_idx[2];
	unsigned int active_users;
	bool virtual_policy;
};

static unsigned int cpu_c0_rep_cpu = 0;
static unsigned int cpu_c2_rep_cpu = 6;
static unsigned int cpu_c3_rep_cpu = 7;

module_param(cpu_c0_rep_cpu, uint, 0444);
module_param(cpu_c2_rep_cpu, uint, 0444);
module_param(cpu_c3_rep_cpu, uint, 0444);

MODULE_PARM_DESC(cpu_c0_rep_cpu, "Representative CPU for the physical A55 policy (CPU0-5)");
MODULE_PARM_DESC(cpu_c2_rep_cpu, "Representative CPU for the physical A76 policy (CPU6)");
MODULE_PARM_DESC(cpu_c3_rep_cpu, "Representative CPU for the virtual CPU7 A76 policy");

#define MAX_OC_PERCENT_OVER_STOCK  60
#define MAX_OC_ABSOLUTE_KHZ       2600000U

static unsigned int cpu_c0_target_khz;
static unsigned int cpu_c2_target_khz;
static unsigned int cpu_c3_target_khz;

/* CPU voltage follows frequency for idx0 when the target is above stock. */
static unsigned int cpu_volt_follow        = 1;
static unsigned int cpu_volt_max_delta_raw = CPU_VOLT_DELTA_DEFAULT_RAW;
static unsigned int cpu_volt_abs_max_raw   = CPU_VOLT_ABS_DEFAULT_RAW;

module_param(cpu_volt_follow, uint, 0644);
MODULE_PARM_DESC(cpu_volt_follow,
		 "1=raise idx0 LUT voltage with frequency, 0=frequency only");

module_param(cpu_volt_max_delta_raw, uint, 0644);
MODULE_PARM_DESC(cpu_volt_max_delta_raw,
		 "Maximum LUT voltage raise over stock idx0, raw (mV*100), default 20000 = +200mV");

module_param(cpu_volt_abs_max_raw, uint, 0644);
MODULE_PARM_DESC(cpu_volt_abs_max_raw,
		 "Ceiling for final idx0 LUT voltage, raw (mV*100), default 112000 = 1.12V, hard max 115000");

/*
 * Explicit idx0 LUT voltage per physical domain, raw (mV*100). 0 = derive it
 * from the stock LUT slope (default). A non-zero value replaces the slope
 * result for that domain: c0 = A55 (CPU0-5), c2 = A76 (shared by CPU6/CPU7).
 * It is rounded up to the 625-raw EEM step, never goes below stock idx0, and
 * is still checked against cpu_volt_max_delta_raw and cpu_volt_abs_max_raw.
 * It takes effect on the next OC apply.
 */
static unsigned int cpu_c0_volt_raw;
static unsigned int cpu_c2_volt_raw;

module_param(cpu_c0_volt_raw, uint, 0644);
MODULE_PARM_DESC(cpu_c0_volt_raw,
		 "Explicit idx0 LUT voltage for the A55 domain, raw (mV*100); 0 = auto from LUT slope");

module_param(cpu_c2_volt_raw, uint, 0644);
MODULE_PARM_DESC(cpu_c2_volt_raw,
		 "Explicit idx0 LUT voltage for the A76 domain (CPU6/CPU7), raw (mV*100); 0 = auto from LUT slope");

/* Effective absolute ceiling: the tunable value clamped to the hard limit. */
static unsigned int cpu_volt_abs_max(void)
{
	return min_t(unsigned int, cpu_volt_abs_max_raw,
		     CPU_VOLT_ABS_HARD_MAX_RAW);
}

/* Details of the last voltage refusal, reported through cpu_oc_result. */
static unsigned int g_volt_refuse_cpu;
static unsigned int g_volt_refuse_need;
static unsigned int g_volt_refuse_top;

static unsigned int g_c0_orig_volt;
static unsigned int g_c2_orig_volt;

static unsigned int g_c0_requested_min_khz;
static unsigned int g_c2_requested_min_khz;
static unsigned int g_c3_requested_min_khz;

static int cpu_oc_apply;

module_param(cpu_c0_target_khz, uint, 0644);
MODULE_PARM_DESC(cpu_c0_target_khz, "A55 CPU0-5 target max frequency KHz (0=stock)");

module_param(cpu_c2_target_khz, uint, 0644);
MODULE_PARM_DESC(cpu_c2_target_khz, "A76 CPU6 target max frequency KHz (0=stock)");

module_param(cpu_c3_target_khz, uint, 0644);
MODULE_PARM_DESC(cpu_c3_target_khz, "A76 CPU7 target max frequency KHz (0=stock)");

static char cpu_oc_result[256] = "not applied yet";

static int cpu_oc_result_get(char *buf, const struct kernel_param *kp)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", cpu_oc_result);
}

static const struct kernel_param_ops cpu_oc_result_ops = {
	.get = cpu_oc_result_get,
};

module_param_cb(cpu_oc_result, &cpu_oc_result_ops, NULL, 0444);
MODULE_PARM_DESC(cpu_oc_result, "CPU control status (read-only)");

static unsigned int g_c0_orig_khz;
static unsigned int g_c2_orig_khz;
static bool g_c0_have_orig;
static bool g_c2_have_orig;

#define QUIESCE_POLL_US     500U
#define QUIESCE_TIMEOUT_US  250000U

static struct cpufreq_policy *get_a76_sibling(struct cpufreq_policy *c2_policy)
{
	struct cpufreq_policy *sib;

	if (cpu_c3_rep_cpu == cpu_c2_rep_cpu)
		return NULL;

	sib = cpufreq_cpu_get(cpu_c3_rep_cpu);
	if (!sib)
		return NULL;

	if (sib == c2_policy || !sib->freq_table) {
		cpufreq_cpu_put(sib);
		return NULL;
	}

	return sib;
}

/*
 * Repair for a bug in the 6+1+1 cpufreq-hw patch.
 *
 * The driver writes PERF_STATE = min(requested_idx[0], requested_idx[1]).
 * Slot [1] belongs to the virtual CPU7 policy, which only exists in the A76
 * domain. The A55 domain has no virtual policy, but policy init still fills
 * slot [1] with the boot-time index and nothing ever updates it again. If that
 * value is 0 the register is always written as 0: the A55 domain is pinned at
 * idx0 (maximum frequency) and can never leave it, so quiesce times out.
 *
 * Make the unused slot neutral (UINT_MAX) and re-sync the register with the
 * governor's last request. Safe to call repeatedly.
 */
static void neutralize_unused_request_slot(unsigned int rep_cpu)
{
	struct cpufreq_policy *policy;
	struct cpufreq_mtk_mirror *c, *hw;
	unsigned long flags;
	unsigned int idx;

	policy = cpufreq_cpu_get(rep_cpu);
	if (!policy)
		return;

	c = (struct cpufreq_mtk_mirror *)policy->driver_data;
	if (!c || c->virtual_policy)
		goto out;

	hw = (c->shared && c->shared != c) ? c->shared : c;

	spin_lock_irqsave(&hw->lock, flags);
	if (hw->requested_idx[1] != UINT_MAX) {
		pr_info("oc_mt6789: cpu%u: unused request slot was %u, set neutral (A55 was pinned to min(req, slot))\n",
			rep_cpu, hw->requested_idx[1]);
		hw->requested_idx[1] = UINT_MAX;
		idx = hw->requested_idx[0];
		if (idx < (unsigned int)hw->nr_opp)
			writel_relaxed(idx, hw->reg_bases[REG_FREQ_PERF_STATE]);
	}
	spin_unlock_irqrestore(&hw->lock, flags);
out:
	cpufreq_cpu_put(policy);
}

/*
 * idx0 LUT voltage this module last wrote, per physical domain (0 = A55,
 * 1 = A76). cpu_lut_dump compares it with what the LUT holds now, which shows
 * whether EEM/SVS (eem_lite) rewrote the voltage field afterwards.
 */
static unsigned int g_applied_volt[2];
static bool g_applied_volt_valid[2];

/*
 * EEM/SVS (eem_lite) periodically rewrites the voltage field of the LUT rows
 * from its own table, which still holds the stock idx0 voltage. When that
 * happens the OC frequency stays but the raised voltage is lost. With
 * cpu_volt_reassert=1 the guardian notices the drift and re-applies the OC
 * (same quiesce + write path as a normal apply), at most once per
 * VOLT_REASSERT_MIN_MS per domain.
 */
#define VOLT_REASSERT_MIN_MS 5000U
static unsigned int cpu_volt_reassert = 1;
static unsigned long g_reassert_last[2];
static unsigned int g_reassert_count[2];

module_param(cpu_volt_reassert, uint, 0644);
MODULE_PARM_DESC(cpu_volt_reassert,
		 "1 = re-apply the idx0 LUT voltage when EEM/SVS rewrites it (default), 0 = off");

/* Details of the last quiesce timeout, reported through cpu_oc_result. */
static unsigned int g_quiesce_cpu;
static unsigned int g_quiesce_state;

/*
 * Actively move one logical policy off idx0.
 *
 * The cpufreq driver only writes REG_FREQ_PERF_STATE when cpufreq actually
 * requests a new frequency. With schedutil + fast switch, a lowered max (our
 * QoS request) is only noticed on the next scheduler update of that policy,
 * so an idle CPU6/CPU7 never leaves idx0 by itself. On the shared A76 domain
 * the register holds min(request CPU6, request CPU7), so BOTH logical
 * policies must have dropped their request.
 *
 * The QoS notifier applies the new limits asynchronously via policy->update,
 * so flush that first; otherwise the target below would still be clamped to
 * the old policy->max.
 */
static void push_policy_off_idx0(struct cpufreq_policy *p)
{
	if (!p || !p->freq_table)
		return;

	flush_work(&p->update);
	cpufreq_driver_target(p, p->freq_table[1].frequency, CPUFREQ_RELATION_H);
}

static int quiesce_off_idx0(struct cpufreq_policy *policy,
			     struct cpufreq_policy *sib,
			     struct cpufreq_mtk_mirror *c,
			     struct freq_qos_request *qos_req)
{
	unsigned int waited_us = 0;
	unsigned int state;
	int ret;

	if (!c || c->nr_opp < 2)
		return -ENODATA;

	ret = freq_qos_add_request(&policy->constraints, qos_req,
				   FREQ_QOS_MAX, policy->freq_table[1].frequency);
	if (ret < 0)
		return ret;

	push_policy_off_idx0(policy);
	push_policy_off_idx0(sib);

	while (waited_us < QUIESCE_TIMEOUT_US) {
		if (readl_relaxed(c->reg_bases[REG_FREQ_PERF_STATE]) != 0)
			return 0;

		usleep_range(QUIESCE_POLL_US, QUIESCE_POLL_US * 2);
		waited_us += QUIESCE_POLL_US;
	}

	state = readl_relaxed(c->reg_bases[REG_FREQ_PERF_STATE]);
	g_quiesce_cpu = cpumask_first(policy->related_cpus);
	g_quiesce_state = state;
	pr_err("oc_mt6789: cpu%u: PERF_STATE still %u after %u us, policy max=%u cur=%u\n",
	       g_quiesce_cpu, state, waited_us, policy->max, policy->cur);

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
	top->frequency = use_khz;
	return 0;
}

/*
 * Change idx0 for one physical hardware domain.
 *
 * A55: CPU0-5 is one physical domain, so only cpu0 is a hardware owner.
 * A76: CPU6 is the physical owner; CPU7 is a virtual policy and shares
 * the same LUT/table. Both logical policies receive the new max.
 */
static int patch_physical_idx0(unsigned int rep_cpu, unsigned int target_khz,
			       unsigned int *orig_khz, unsigned int *orig_volt,
			       bool *have_orig)
{
	struct cpufreq_policy *policy;
	struct cpufreq_policy *sib = NULL;
	struct cpufreq_mtk_mirror *c;
	struct cpufreq_mtk_mirror *hw;
	struct freq_qos_request qos_req;
	struct freq_qos_request sib_qos;
	unsigned int use_khz, cap_khz, old_max, sib_old_max;
	unsigned int new_volt = 0;
	unsigned int ovr;
	s64 dv = 0;
	bool sib_qos_held = false;
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
	hw = (c->shared && c->shared != c) ? c->shared : c;

	if (c->virtual_policy) {
		cpufreq_cpu_put(policy);
		return -EINVAL;
	}

	if (rep_cpu == cpu_c0_rep_cpu)
		neutralize_unused_request_slot(rep_cpu);

	if (!*have_orig) {
		*orig_khz = hw->table[0].frequency;
		raw = readl_relaxed(hw->reg_bases[REG_FREQ_LUT_TABLE]);
		*orig_volt = FIELD_GET(LUT_VOLT, raw);
		*have_orig = true;
	}

	if (!target_khz)
		use_khz = *orig_khz;
	else {
		cap_khz = *orig_khz +
			  (*orig_khz * MAX_OC_PERCENT_OVER_STOCK) / 100;
		if (cap_khz > MAX_OC_ABSOLUTE_KHZ)
			cap_khz = MAX_OC_ABSOLUTE_KHZ;
		if (target_khz > cap_khz) {
			cpufreq_cpu_put(policy);
			return -ERANGE;
		}
		use_khz = target_khz;
	}

	/*
	 * Derive voltage only from this domain's own stock LUT rows 1 and 3.
	 * Round upward to the EEM step so frequency increases never under-volt.
	 * No hardware write occurs until all checks pass.
	 */
	ovr = (rep_cpu == cpu_c0_rep_cpu) ? cpu_c0_volt_raw : cpu_c2_volt_raw;

	if ((cpu_volt_follow || ovr) && use_khz > *orig_khz) {
		if (ovr) {
			unsigned int want = DIV_ROUND_UP(ovr, EEM_VOLT_STEP) *
					    EEM_VOLT_STEP;

			dv = want > *orig_volt ? (s64)(want - *orig_volt) : 0;
			pr_info("oc_mt6789: cpu%u: explicit idx0 voltage %u raw requested (stock %u raw, raise +%lld)\n",
				rep_cpu, want, *orig_volt, dv);
		} else if (hw->nr_opp >= 4) {
			u32 r1 = readl_relaxed(hw->reg_bases[REG_FREQ_LUT_TABLE] +
					       1 * LUT_ROW_SIZE);
			u32 r3 = readl_relaxed(hw->reg_bases[REG_FREQ_LUT_TABLE] +
					       3 * LUT_ROW_SIZE);
			int df = (int)FIELD_GET(LUT_FREQ, r1) -
				 (int)FIELD_GET(LUT_FREQ, r3);
			int dvs = (int)FIELD_GET(LUT_VOLT, r1) -
				  (int)FIELD_GET(LUT_VOLT, r3);

			if (df > 0 && dvs > 0) {
				dv = ((s64)dvs *
				      (int)(use_khz / 1000 - *orig_khz / 1000)) / df;
				dv = ((dv + EEM_VOLT_STEP - 1) /
				      EEM_VOLT_STEP) * EEM_VOLT_STEP;
			} else {
				pr_warn("oc_mt6789: cpu%u: no usable voltage slope in LUT rows 1/3; frequency only\n",
					rep_cpu);
			}
		} else {
			pr_warn("oc_mt6789: cpu%u: nr_opp<4, cannot derive voltage slope; frequency only\n",
				rep_cpu);
		}

		if (dv > (s64)cpu_volt_max_delta_raw ||
		    (s64)*orig_volt + dv > (s64)cpu_volt_abs_max()) {
			pr_err("oc_mt6789: cpu%u: voltage raise +%lld raw (top %lld raw) exceeds delta limit %u or abs max %u; refused\n",
			       rep_cpu, dv, (s64)*orig_volt + dv,
			       cpu_volt_max_delta_raw, cpu_volt_abs_max());
			g_volt_refuse_cpu  = rep_cpu;
			g_volt_refuse_need = (unsigned int)dv;
			g_volt_refuse_top  = (unsigned int)((s64)*orig_volt + dv);
			if (*have_orig)
				*have_orig = false;
			cpufreq_cpu_put(policy);
			return -EOVERFLOW;
		}
	}

	new_volt = *orig_volt + (unsigned int)dv;

	old_max = policy->max;

	/* CPU6/CPU7 are separate logical policies over one A76 hardware domain. */
	if (rep_cpu == cpu_c2_rep_cpu)
		sib = get_a76_sibling(policy);

	if (sib && c->nr_opp >= 2) {
		ret = freq_qos_add_request(&sib->constraints, &sib_qos,
					   FREQ_QOS_MAX,
					   sib->freq_table[1].frequency);
		if (ret < 0) {
			cpufreq_cpu_put(sib);
			cpufreq_cpu_put(policy);
			return ret;
		}
		sib_qos_held = true;
	}

	ret = quiesce_off_idx0(policy, sib, hw, &qos_req);
	if (ret)
		goto out;

	/* One 32-bit LUT word carries both idx0 frequency and candidate voltage. */
	raw = readl_relaxed(hw->reg_bases[REG_FREQ_LUT_TABLE]);
	raw = (raw & ~(LUT_FREQ | LUT_VOLT)) |
	      FIELD_PREP(LUT_FREQ, use_khz / 1000) |
	      FIELD_PREP(LUT_VOLT, new_volt);
	writel_relaxed(raw, hw->reg_bases[REG_FREQ_LUT_TABLE]);

	g_applied_volt[rep_cpu == cpu_c0_rep_cpu ? 0 : 1] = new_volt;
	g_applied_volt_valid[rep_cpu == cpu_c0_rep_cpu ? 0 : 1] = true;

	hw->table[0].frequency = use_khz;

	down_write(&policy->rwsem);
	policy->freq_table[0].frequency = use_khz;
	policy->cpuinfo.max_freq = use_khz;
	policy->max = use_khz;
	if (policy->min > use_khz || policy->min == old_max)
		policy->min = use_khz;
	up_write(&policy->rwsem);

	if (sib) {
		down_write(&sib->rwsem);
		sib_old_max = sib->max;
		sib->freq_table[0].frequency = use_khz;
		sib->cpuinfo.max_freq = use_khz;
		sib->max = use_khz;
		if (sib->min > use_khz || sib->min == sib_old_max)
			sib->min = use_khz;
		up_write(&sib->rwsem);
	}

	freq_qos_remove_request(&qos_req);
	if (sib_qos_held)
		freq_qos_remove_request(&sib_qos);
	sib_qos_held = false;

	if (patch_em_table(rep_cpu, use_khz))
		pr_warn("oc_mt6789: cpu%u has no EM perf domain\n", rep_cpu);

	if (sib) {
		if (patch_em_table(cpu_c3_rep_cpu, use_khz))
			pr_warn("oc_mt6789: cpu%u has no EM perf domain\n",
				cpu_c3_rep_cpu);
	}

	pr_info("oc_mt6789: physical domain cpu%u max=%u KHz, LUT volt %u->%u raw%s\n",
		rep_cpu, use_khz, *orig_volt, new_volt,
		sib ? " (CPU7 logical sibling updated)" : "");

	ret = 0;
	goto out_put;

out:
	freq_qos_remove_request(&qos_req);
	if (sib_qos_held)
		freq_qos_remove_request(&sib_qos);
out_put:
	if (sib)
		cpufreq_cpu_put(sib);
	cpufreq_cpu_put(policy);
	return ret;
}

static void set_logical_a76_max(unsigned int rep_cpu, unsigned int max_khz)
{
	struct cpufreq_policy *policy;

	if (!max_khz)
		return;

	policy = cpufreq_cpu_get(rep_cpu);
	if (!policy)
		return;

	down_write(&policy->rwsem);
	if (max_khz > policy->cpuinfo.max_freq)
		max_khz = policy->cpuinfo.max_freq;
	policy->max = max_khz;
	if (policy->min > policy->max)
		policy->min = policy->max;
	up_write(&policy->rwsem);

	cpufreq_cpu_put(policy);
}

/* Apply the current CPU OC targets. Caller must hold oc_lock. */
static void cpu_oc_apply_locked(void)
{
	unsigned int a76_physical_target;
	int ret0 = 0, ret2 = 0;

	/*
	 * CPU0-5 has its own physical LUT. CPU6/CPU7 are separate logical
	 * policies but share one physical A76 LUT, so the physical LUT must
	 * be large enough for the higher of the two requested targets.
	 */
	a76_physical_target = cpu_c2_target_khz;
	if (cpu_c3_target_khz > a76_physical_target)
		a76_physical_target = cpu_c3_target_khz;

	if (cpu_c0_target_khz || g_c0_have_orig)
		ret0 = patch_physical_idx0(cpu_c0_rep_cpu, cpu_c0_target_khz,
					   &g_c0_orig_khz, &g_c0_orig_volt,
					   &g_c0_have_orig);

	if (!ret0 && (a76_physical_target || g_c2_have_orig))
		ret2 = patch_physical_idx0(cpu_c2_rep_cpu, a76_physical_target,
					   &g_c2_orig_khz, &g_c2_orig_volt,
					   &g_c2_have_orig);

	if (!ret0 && !ret2) {
		/*
		 * The shared physical LUT follows the higher A76 request, while
		 * each Linux policy keeps its own logical max.
		 */
		if (g_c2_have_orig) {
			unsigned int c2_max = cpu_c2_target_khz ?
					      cpu_c2_target_khz : g_c2_orig_khz;
			unsigned int c3_max = cpu_c3_target_khz ?
					      cpu_c3_target_khz : g_c2_orig_khz;

			set_logical_a76_max(cpu_c2_rep_cpu, c2_max);
			set_logical_a76_max(cpu_c3_rep_cpu, c3_max);
		}
	}

	if (ret0 == -ERANGE || ret2 == -ERANGE)
		snprintf(cpu_oc_result, sizeof(cpu_oc_result),
			 "FAIL: target exceeds safety cap (A55=%u CPU6=%u CPU7=%u KHz)",
			 cpu_c0_target_khz, cpu_c2_target_khz, cpu_c3_target_khz);
	else if (ret0 == -EOVERFLOW || ret2 == -EOVERFLOW)
		snprintf(cpu_oc_result, sizeof(cpu_oc_result),
			 "FAIL: cpu%u needs +%u raw volt (top %u raw); limits delta=%u abs=%u. Raise cpu_volt_max_delta_raw / cpu_volt_abs_max_raw or lower the target",
			 g_volt_refuse_cpu, g_volt_refuse_need, g_volt_refuse_top,
			 cpu_volt_max_delta_raw, cpu_volt_abs_max());
	else if (ret0 == -ETIMEDOUT || ret2 == -ETIMEDOUT)
		snprintf(cpu_oc_result, sizeof(cpu_oc_result),
			 "FAIL: domain of cpu%u stuck at PERF_STATE=%u, did not leave idx0 (see dmesg oc_mt6789)",
			 g_quiesce_cpu, g_quiesce_state);
	else if (ret0 || ret2)
		snprintf(cpu_oc_result, sizeof(cpu_oc_result),
			 "FAIL: A55=%d A76=%d", ret0, ret2);
	else
		snprintf(cpu_oc_result, sizeof(cpu_oc_result),
			 "OK: A55=%u CPU6=%u CPU7=%u KHz (A76 physical=%u)",
			 cpu_c0_target_khz ? cpu_c0_target_khz : g_c0_orig_khz,
			 cpu_c2_target_khz ? cpu_c2_target_khz : g_c2_orig_khz,
			 cpu_c3_target_khz ? cpu_c3_target_khz : g_c2_orig_khz,
			 a76_physical_target ? a76_physical_target : g_c2_orig_khz);

}

static int cpu_oc_apply_set(const char *val, const struct kernel_param *kp)
{
	unsigned int trigger;

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
	cpu_oc_apply_locked();
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
MODULE_PARM_DESC(cpu_oc_apply,
		 "Write 1 to apply A55, CPU6 and CPU7 logical max targets");

static int cpu_min_khz_set(unsigned int rep_cpu, unsigned int *requested,
			   const char *val)
{
	struct cpufreq_policy *policy;
	unsigned int khz;

	if (kstrtouint(val, 10, &khz) || khz == 0)
		return -EINVAL;

	policy = cpufreq_cpu_get(rep_cpu);
	if (!policy)
		return -ENODEV;

	mutex_lock(&oc_lock);

	down_write(&policy->rwsem);
	if (khz > policy->max)
		khz = policy->max;
	if (khz < policy->cpuinfo.min_freq)
		khz = policy->cpuinfo.min_freq;

	*requested = khz;
	policy->min = khz;
	up_write(&policy->rwsem);

	mutex_unlock(&oc_lock);
	cpufreq_cpu_put(policy);
	return 0;
}

static int cpu_min_khz_get(unsigned int rep_cpu, unsigned int requested,
			   char *buf)
{
	struct cpufreq_policy *policy;
	unsigned int value = requested;

	if (value)
		return scnprintf(buf, PAGE_SIZE, "%u\n", value);

	policy = cpufreq_cpu_get(rep_cpu);
	if (!policy)
		return scnprintf(buf, PAGE_SIZE, "0\n");

	value = policy->min;
	cpufreq_cpu_put(policy);
	return scnprintf(buf, PAGE_SIZE, "%u\n", value);
}

static int cpu_c0_min_khz_set(const char *val, const struct kernel_param *kp)
{
	return cpu_min_khz_set(cpu_c0_rep_cpu, &g_c0_requested_min_khz, val);
}

static int cpu_c0_min_khz_get(char *buf, const struct kernel_param *kp)
{
	return cpu_min_khz_get(cpu_c0_rep_cpu, g_c0_requested_min_khz, buf);
}

static const struct kernel_param_ops cpu_c0_min_khz_ops = {
	.set = cpu_c0_min_khz_set,
	.get = cpu_c0_min_khz_get,
};

module_param_cb(cpu_c0_min_khz, &cpu_c0_min_khz_ops, NULL, 0644);
MODULE_PARM_DESC(cpu_c0_min_khz, "Requested minimum for A55 CPU0-5");

static int cpu_c2_min_khz_set(const char *val, const struct kernel_param *kp)
{
	return cpu_min_khz_set(cpu_c2_rep_cpu, &g_c2_requested_min_khz, val);
}

static int cpu_c2_min_khz_get(char *buf, const struct kernel_param *kp)
{
	return cpu_min_khz_get(cpu_c2_rep_cpu, g_c2_requested_min_khz, buf);
}

static const struct kernel_param_ops cpu_c2_min_khz_ops = {
	.set = cpu_c2_min_khz_set,
	.get = cpu_c2_min_khz_get,
};

module_param_cb(cpu_c2_min_khz, &cpu_c2_min_khz_ops, NULL, 0644);
MODULE_PARM_DESC(cpu_c2_min_khz, "Requested minimum for CPU6 A76 policy");

static int cpu_c3_min_khz_set(const char *val, const struct kernel_param *kp)
{
	return cpu_min_khz_set(cpu_c3_rep_cpu, &g_c3_requested_min_khz, val);
}

static int cpu_c3_min_khz_get(char *buf, const struct kernel_param *kp)
{
	return cpu_min_khz_get(cpu_c3_rep_cpu, g_c3_requested_min_khz, buf);
}

static const struct kernel_param_ops cpu_c3_min_khz_ops = {
	.set = cpu_c3_min_khz_set,
	.get = cpu_c3_min_khz_get,
};

module_param_cb(cpu_c3_min_khz, &cpu_c3_min_khz_ops, NULL, 0644);
MODULE_PARM_DESC(cpu_c3_min_khz,
		 "Requested minimum for CPU7 A76 virtual policy");

#define GUARDIAN_POLL_MS 50

static struct task_struct *g_guardian_thread;

static void guardian_check_one(unsigned int rep_cpu, unsigned int target_khz,
			       unsigned int requested_min)
{
	struct cpufreq_policy *policy;
	unsigned int min_khz;

	policy = cpufreq_cpu_get(rep_cpu);
	if (!policy)
		return;

	down_write(&policy->rwsem);

	if (target_khz) {
		if (policy->max != target_khz)
			policy->max = target_khz;
		if (policy->cpuinfo.max_freq != target_khz)
			policy->cpuinfo.max_freq = target_khz;
	}

	if (requested_min) {
		min_khz = requested_min;
		if (min_khz > policy->max)
			min_khz = policy->max;
		if (min_khz < policy->cpuinfo.min_freq)
			min_khz = policy->cpuinfo.min_freq;
		policy->min = min_khz;
	}

	if (policy->min > policy->max)
		policy->min = policy->max;

	up_write(&policy->rwsem);
	cpufreq_cpu_put(policy);
}

/* Current idx0 LUT voltage of the physical domain behind rep_cpu. */
static bool lut_idx0_volt(unsigned int rep_cpu, unsigned int *volt)
{
	struct cpufreq_policy *policy = cpufreq_cpu_get(rep_cpu);
	struct cpufreq_mtk_mirror *c, *hw;
	bool ok = false;

	if (!policy)
		return false;

	c = (struct cpufreq_mtk_mirror *)policy->driver_data;
	if (c) {
		hw = (c->shared && c->shared != c) ? c->shared : c;
		*volt = FIELD_GET(LUT_VOLT,
				  readl_relaxed(hw->reg_bases[REG_FREQ_LUT_TABLE]));
		ok = true;
	}
	cpufreq_cpu_put(policy);
	return ok;
}

/* Caller holds oc_lock. Re-apply the OC if EEM/SVS rewrote an idx0 voltage. */
static void guardian_reassert_volt(void)
{
	unsigned int a76_target = max(cpu_c2_target_khz, cpu_c3_target_khz);
	bool need = false;
	int dom;

	if (!cpu_volt_reassert || atomic_read(&oc_mt6789_suspended))
		return;

	for (dom = 0; dom < 2; dom++) {
		unsigned int rep = dom ? cpu_c2_rep_cpu : cpu_c0_rep_cpu;
		unsigned int tgt = dom ? a76_target : cpu_c0_target_khz;
		unsigned int orig = dom ? g_c2_orig_khz : g_c0_orig_khz;
		bool have = dom ? g_c2_have_orig : g_c0_have_orig;
		unsigned int now;

		if (!tgt || !have || tgt <= orig || !g_applied_volt_valid[dom])
			continue;
		if (!lut_idx0_volt(rep, &now) || now == g_applied_volt[dom])
			continue;
		if (time_before(jiffies, g_reassert_last[dom] +
				msecs_to_jiffies(VOLT_REASSERT_MIN_MS)))
			continue;

		g_reassert_last[dom] = jiffies;
		g_reassert_count[dom]++;
		pr_info("oc_mt6789: %s idx0 voltage rewritten %u -> %u raw (EEM/SVS?), re-applying (#%u)\n",
			dom ? "A76" : "A55", g_applied_volt[dom], now,
			g_reassert_count[dom]);
		need = true;
	}

	if (need)
		cpu_oc_apply_locked();
}

static int guardian_thread_fn(void *unused)
{
	unsigned int tick = 0;

	while (!kthread_should_stop()) {
		mutex_lock(&oc_lock);
		guardian_check_one(cpu_c0_rep_cpu, cpu_c0_target_khz,
				   g_c0_requested_min_khz);
		guardian_check_one(cpu_c2_rep_cpu, cpu_c2_target_khz,
				   g_c2_requested_min_khz);
		guardian_check_one(cpu_c3_rep_cpu, cpu_c3_target_khz,
				   g_c3_requested_min_khz);
		if ((++tick % 10) == 0)
			guardian_reassert_volt();
		mutex_unlock(&oc_lock);
		msleep_interruptible(GUARDIAN_POLL_MS);
	}
	return 0;
}

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
		"[idx] sw_freq(RAM)   hw_freq(MMIO)   lut_volt(raw)   match?\n");

	for (i = 0; i < c->nr_opp && i < LUT_MAX_ENTRIES; i++) {
		unsigned int sw_freq = policy->freq_table[i].frequency;
		u32 raw = readl_relaxed(c->reg_bases[REG_FREQ_LUT_TABLE] + (i * LUT_ROW_SIZE));
		unsigned int hw_freq = FIELD_GET(LUT_FREQ, raw) * 1000;
		unsigned int lut_volt = FIELD_GET(LUT_VOLT, raw);
		const char *mark = (i == cur_idx) ? "*" : " ";
		const char *match = (sw_freq == hw_freq) ? "OK" : "MISMATCH";

		*off += scnprintf(buf + *off, bufsize - *off,
			"[%2d]%s %10u KHz  %10u KHz   %8u   %s\n",
			i, mark, sw_freq, hw_freq, lut_volt, match);

		if (*off >= bufsize - 256)
			break;
	}

	{
		int dom = (cpu == (int)cpu_c0_rep_cpu) ? 0 : 1;
		struct cpufreq_mtk_mirror *hw =
			(c->shared && c->shared != c) ? c->shared : c;
		u32 raw0 = readl_relaxed(c->reg_bases[REG_FREQ_LUT_TABLE]);
		unsigned int now_volt = FIELD_GET(LUT_VOLT, raw0);

		*off += scnprintf(buf + *off, bufsize - *off,
			"logical policies: %s | request slots [%u,%u]\n",
			dom ? "cpu6 + cpu7 (virtual, shares this LUT)" :
			      "cpu0-5 (single policy, slot[1] unused)",
			hw->requested_idx[0], hw->requested_idx[1]);

		if (g_applied_volt_valid[dom])
			*off += scnprintf(buf + *off, bufsize - *off,
				"idx0 volt: written by module=%u, LUT now=%u -> %s | re-applied %ux\n",
				g_applied_volt[dom], now_volt,
				now_volt == g_applied_volt[dom] ? "HELD" :
				"CHANGED after write (EEM/SVS rewrote it?)",
				g_reassert_count[dom]);
		else
			*off += scnprintf(buf + *off, bufsize - *off,
				"idx0 volt: no OC applied yet, LUT now=%u\n", now_volt);
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

static int __init oc_mt6789_init(void)
{
	unsigned long addr;
	int ret;

	pr_info("overclock_mt6789: init (GPU working_table patch + CPU cpufreq-hw LUT patch)\n");
	pr_info("oc_mt6789: 6+1+1 CPU control safety cap = stock +%d%%, %uMHz absolute ceiling\n",
		MAX_OC_PERCENT_OVER_STOCK, MAX_OC_ABSOLUTE_KHZ / 1000);
	pr_info("oc_mt6789: CPU LUT voltage follow=%u, max raise=%u raw, abs max=%u raw (hard max %u)\n",
		cpu_volt_follow, cpu_volt_max_delta_raw, cpu_volt_abs_max(),
		CPU_VOLT_ABS_HARD_MAX_RAW);

	neutralize_unused_request_slot(cpu_c0_rep_cpu);

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

			sym_mtk_fh_set_rate = (fn_mtk_fh_set_rate_t)addr;
		else
			pr_warn("oc_mt6789: mtk_fh_set_rate not found — GPU PCW change fails safe\n");


		addr = sym_kallsyms_lookup_name("g_working_table");
		if (addr) {
			sym_ged_g_working_table = (struct gpufreq_opp_info **)addr;
			pr_info("oc_mt6789: found GED's g_working_table via kallsyms — current_freqency will reflect OC\n");
		} else {
			pr_warn("oc_mt6789: g_working_table not found via kallsyms (CONFIG_KALLSYMS_ALL off, ged.ko not loaded yet, or gpufreq v1 build) — GPU OC still applies at hardware level, but /sys/kernel/ged/hal/current_freqency will keep showing stock freq\n");
		}


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
	if (g_c2_have_orig)
		patch_physical_idx0(cpu_c2_rep_cpu, 0, &g_c2_orig_khz, &g_c2_orig_volt,
			    &g_c2_have_orig);
	if (g_c0_have_orig)
		patch_physical_idx0(cpu_c0_rep_cpu, 0, &g_c0_orig_khz, &g_c0_orig_volt,
			    &g_c0_have_orig);
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
MODULE_DESCRIPTION("6+1+1 CPU + GPU control for MT6789 Helio G99 — POCO M5 rock");
MODULE_VERSION("2.2.7-6P1P1");
