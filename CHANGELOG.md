# Changelog

## 3 Cluster CPU

Final 6+1+1 logical CPU policy implementation for MT6789:

- CPU0-5: one logical A55 policy over stock physical performance domain 0.
- CPU6: one logical A76 policy over stock physical performance domain 1.
- CPU7: one logical A76 policy sharing the same physical hardware domain and LUT as CPU6.
- Stock two physical performance domains are preserved; no third physical domain or DTB topology change is required.
- The `overclock_mt6789` addon matches the 6+1+1 driver layout and physical-domain ownership.
- Optional CPU LUT voltage follow is included with explicit safety limits and documented hardware-validation caveats.
