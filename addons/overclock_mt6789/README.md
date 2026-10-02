# overclock_mt6789

Out-of-tree kernel module for POCO M5 (rock) / MediaTek MT6789 (Helio G99).

This addon is aligned with the main 6+1+1 cpufreq-hw patch and the stock two-domain DTB.

## CPU topology

Physical A55 domain: CPU0-5
  Linux policy: CPU0-5, representative CPU0

Physical A76 domain: CPU6-7
  Linux policy: CPU6, representative CPU6
  Linux policy: CPU7, representative CPU7 (virtual/shared)

CPU6 and CPU7 are separate Linux policies backed by the same A76 hardware domain and LUT.
CPU0-5 remain one physical A55 policy.

The addon does not split CPU0-5 into CPU0-3 and CPU4-5 and does not treat CPU4 as an independent hardware cluster.

## CPU control

The CPU path operates on the top frequency entry (idx0) of each physical domain.

- A55: CPU0-5, hardware owner CPU0.
- A76: CPU6/CPU7, hardware owner CPU6; CPU7's logical policy is updated with the same top frequency.

Before rewriting a live LUT row, the affected hardware domain is temporarily constrained to idx1 and the module waits for the hardware state to leave idx0. Timeout is 50 ms.

The module updates the hardware LUT entry 0, the shared cpufreq frequency table entry 0, policy max/cpuinfo max, and the Energy Model top frequency where available.

For the A76 split, both CPU6 and CPU7 policy limits are synchronized because they share the same frequency table and hardware domain.

### CPU parameters

All parameters are under /sys/module/overclock_mt6789/parameters/.

- cpu_c0_target_khz: A55 CPU0-5 target max; 0 restores stock.
- cpu_c2_target_khz: A76 CPU6/CPU7 target max; 0 restores stock.
- cpu_oc_apply: write 1 to apply configured CPU targets.
- cpu_oc_result: read-only result of the last CPU apply.
- cpu_c0_min_khz: minimum for CPU0-5 policy.
- cpu_c2_min_khz: minimum for CPU6 policy.
- cpu_c3_min_khz: minimum for CPU7 policy.
- cpu_lut_dump: read-only software-vs-hardware LUT dump.

The old cpu_c1_* interface is intentionally removed because there is no separate CPU4-5 hardware domain in the 6+1+1 topology.

### CPU target limits

The existing software safety limits are retained: stock_idx0 + 60%, with a 2600000 KHz absolute ceiling.

These are software request limits only. They do not guarantee hardware stability or thermal safety. CPU voltage follow is separately controlled by `cpu_volt_follow` and its documented limits below.

## GPU

The existing MT6789 GPU working-table and PLL path is retained. It patches GPU OPP index 0, the signed table when available, GED cache when available, and the MFG PLL through mtk_fh_set_rate().

GPU frequency remains limited to 1750000 KHz. GPU voltage/VSRAM inputs remain limited to 50000..100000 mV×100.

## Runtime compatibility

This addon must be compiled against the same kernel tree/configuration that produced the 6+1+1 cpufreq-hw module.

The addon mirror matches the patched driver's struct cpufreq_mtk layout through virtual_policy. A different vendor driver layout must not be used with this module.

The stock DTB must retain two physical performance domains:
CPU0-5 -> performance domain 0
CPU6-7 -> performance domain 1

Do not create a third DT performance-domain node for this addon.

Required for kallsyms-resolved paths: CONFIG_KALLSYMS_ALL=y.

## Build

Standalone:
make KDIR=~/OSS/common WORKSPACE=~/OSS
make check-clang

Pipeline:
ADDONS=overclock_mt6789 ./build.sh

The addon is intended to be packaged together with the corresponding 6+1+1 cpufreq-hw .ko.

## Safety

The module keeps the suspend/hibernation guard and live-LUT quiesce/timeout handling.
Overclocking is not automatically applied at module load; CPU and GPU target changes require an explicit apply parameter write.

## License

GPL-2.0.

## CPU voltage follow

The 6+1+1 CPU path can optionally raise the voltage field in the same hardware LUT word when idx0 frequency is raised.

Parameters:
- `cpu_volt_follow=1`: enable frequency-following LUT voltage calculation.
- `cpu_volt_max_delta_raw=12500`: maximum increase over stock idx0 voltage (raw, mV*100; 12500 = +125 mV). Runtime tunable.
- `cpu_volt_abs_max_raw=110000`: ceiling for the final idx0 LUT voltage (110000 = 1.10 V). Runtime tunable, but always clamped to a compile-time hard maximum of `115000` raw (1.15 V) that no parameter can lift.

Raw unit is 10 uV, so 625 raw = one 6.25 mV EEM step and 100 raw = 1 mV.

The old defaults (`3000` / `102000`) refused most useful OC targets, because a few hundred MHz of overclock already needs more than +30 mV on the stock LUT slope.

If a target still needs more voltage than the limits allow, the apply is refused (never silently under-volted) and `cpu_oc_result` reports the real numbers, for example:

    FAIL: cpu0 needs +13750 raw volt (top 108750 raw); limits delta=12500 abs=110000. ...

Raise the matching parameter at runtime (no rebuild) or lower the target:

    echo 15000 > /sys/module/overclock_mt6789/parameters/cpu_volt_max_delta_raw
    echo 112000 > /sys/module/overclock_mt6789/parameters/cpu_volt_abs_max_raw

Higher voltage means more heat and faster silicon wear. Raise limits only as far as a target actually needs.

The voltage slope is derived from the same physical domain's stock LUT rows 1 and 3, then rounded upward to the 625-raw EEM step. The write preserves all other bits in the LUT row and changes only `LUT_FREQ` and `LUT_VOLT`.

**Important:** `LUT_VOLT = bits[28:12]` is based on correlation with `/proc/eem_lite/eem_cur_volt`, not an official MediaTek register definition. This implementation therefore treats it as a hardware-LUT voltage candidate, not proof that the PMIC/MCUPM rail has been independently commanded. Validate readback and actual EEM/voltage behavior on-device before relying on it.
