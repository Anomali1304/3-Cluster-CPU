<div align="center">

# 3-Cluster CPU

**Kernel build pipeline and out-of-tree addon framework for Xiaomi POCO M5 (rock), MediaTek MT6789.**

[![Build](https://img.shields.io/github/actions/workflow/status/Anomali1304/3-Cluster-CPU/build.yml?style=for-the-badge&logo=githubactions&logoColor=white&labelColor=222)](https://github.com/Anomali1304/3-Cluster-CPU/actions/workflows/build.yml)
[![Latest release](https://img.shields.io/badge/version-1.0.0-informational?style=for-the-badge&logo=github&logoColor=white&labelColor=222)](CHANGELOG.md)
[![Kernel](https://img.shields.io/badge/kernel-5.10-informational?style=for-the-badge&logoColor=white&labelColor=222)](docs/ARCHITECTURE.md)
[![License](https://img.shields.io/badge/license-GPL--2.0-informational?style=for-the-badge&logo=gnu&logoColor=white&labelColor=222)](LICENSE)

</div>

3-Cluster CPU builds Xiaomi's `android_kernel_xiaomi_rock` tree for the POCO M5 (`rock`, MT6789/Helio G99) and can build optional out-of-tree kernel modules from `addons/` against the exact kernel build.

## Why not just clone and build manually?

The GKI manifest, the Xiaomi tree, root patches, and vendor modules don't line up cleanly out of the box. `build/build.sh` overrides `CLANG_PREBUILT_BIN` and the build user/host regardless of what you export, XClang needs a `libxml2.so.16` symlink to even run, and SUSFS has to be patched in separately from KernelSU-Next's `dev-susfs` fork. This pipeline used to be an interactive script run by hand on a build server; it's now a set of numbered stages so the same steps run the same way every time, locally or in CI.

## Features

- Builds the Xiaomi `android_kernel_xiaomi_rock` tree without manual GKI manifest surgery
- Root integration: Vanilla, KernelSU-Next, or ReSukiSU, with optional SUSFS
- Out-of-tree addon framework, drop a directory in `addons/` and it's part of the build
- Vendor MTK, in-tree GKI, and addon `.ko` files all packaged together per run
- Every option is an env var, defaults live in `config/defaults.env`
- Runs the same locally or through a GitHub Actions `workflow_dispatch`

## Supported Root Solutions

- [Vanilla](kernel/root/vanilla)
- [KernelSU-Next](kernel/root/ksun)
- [ReSukiSU](kernel/root/resukisu)

## Quick Start

```bash
./build.sh

ADDONS=overclock_mt6789 ROOT_TYPE=KSUN ./build.sh

MODULES_FILTER=gpufreq ./build.sh

RUN_MODE="Dry Run" ./build.sh
```

All build options are environment variables. Defaults are defined in [`config/defaults.env`](config/defaults.env).

## Repository Layout

```text
3-Cluster-CPU/
├── build.sh
├── config/
│   └── defaults.env
├── lib/
│   ├── functions.sh
│   └── paths.sh
├── stages/
│   ├── 00-deps.sh
│   ├── 10-download.sh
│   ├── 15-clang-vendor.sh
│   ├── 20-branding.sh
│   ├── 30-core.sh
│   ├── 40-root-variant.sh
│   ├── 50-addons-stage.sh
│   ├── 60-build-kernel.sh
│   ├── 70-addons-compile.sh
│   └── 80-release.sh
├── kernel/
│   ├── core/
│   └── root/
├── addons/
│   └── overclock_mt6789/
├── release/
├── docs/
├── .github/workflows/build.yml
├── CHANGELOG.md
├── LICENSE
└── VERSION
```

## Pipeline

| Stage | Responsibility |
|---|---|
| `00-deps` | Host dependencies and `repo` launcher |
| `10-download` | GKI tooling, Xiaomi kernel source, optional vendor modules |
| `15-clang-vendor` | GKI, ZyC, or custom Clang selection |
| `20-branding` | Kernel version and build identity |
| `30-core` | LTO and size/debug configuration |
| `40-root-variant` | Root integration |
| `50-addons-stage` | Validate and stage enabled addons |
| `60-build-kernel` | Kernel compilation |
| `70-addons-compile` | Compile staged addons against the completed kernel |
| `80-release` | Build release archives |

`RUN_MODE="Dry Run"` stops after the kernel build stage. The normal run continues through addon compilation and packaging.

## Outputs

Artifacts are written to `release-out/`.

| Output | Condition | Contents |
|---|---|---|
| `<prefix><root>.zip` | Always | AnyKernel3 package containing the kernel image |
| `<prefix><root>-addons.zip` | `ADDONS` set and modules built | Enabled addon `.ko` files |
| `<prefix><root>-modules.zip` | At least one `.ko` built | Vendor, GKI, and addon `.ko` files; optionally filtered by `MODULES_FILTER` |

The kernel image package does not contain `.ko` files.

## Configuration

| Variable | Default | Values / purpose |
|---|---|---|
| `KERNEL_NAME` | `RockKernel` | `CONFIG_LOCALVERSION` suffix |
| `ZIP_PREFIX` | `Rock-Kernel` | Release filename prefix |
| `BUILD_USER_NAME` | `MaL` | Kernel build user |
| `BUILD_HOST_NAME` | `1304` | Kernel build host |
| `KERNEL_REPO` | Xiaomi kernel repo | Kernel source |
| `KERNEL_BRANCH` | `android12-5.10` | Kernel source branch |
| `GKI_MANIFEST_BRANCH` | `common-android12-5.10-lts` | GKI tooling manifest |
| `VENDOR_MODULES_REPO` | MTK modules repo | External vendor modules |
| `VENDOR_MODULES_BRANCH` | `rock-u-oss` | Vendor module branch |
| `USE_EXT_MODULES` | `y` | Build external vendor modules |
| `LTO_MODE` | `THIN` | `NONE`, `THIN`, `FULL` |
| `ROOT_TYPE` | `VANILLA` | `VANILLA`, `KSUN`, `RESUKISU` |
| `STRIP_DEBUG` | `y` | Debug/tracing reduction |
| `CLANG_VENDOR` | `GKI` | `GKI`, `ZyC`, `Custom` |
| `CLANG_CUSTOM_URL` | empty | Custom Clang archive URL |
| `CLANG_CUSTOM_PATH` | empty | Local custom Clang archive |
| `ADDONS` | empty | Comma-separated addon names |
| `MODULES_FILTER` | empty | Case-insensitive filename substring filter |
| `RUN_MODE` | `Full Run` | `Full Run` or `Dry Run` |
| The cpufreq topology is fixed to three logical policies (6+1+1) over the stock two physical hardware domains; no topology toggle is required |

The authoritative defaults are the values in [`config/defaults.env`](config/defaults.env).

## Addons

Addons are self-contained under `addons/<n>/` and do not modify the kernel tree.

Required files:

```text
addons/<n>/
├── <n>.c
├── Makefile
└── README.md
```

The directory name, module source name, and `obj-m` target must match. The generic addon stages require no addon-specific changes.

See [`docs/ADDONS.md`](docs/ADDONS.md).

### `overclock_mt6789`

`overclock_mt6789` is the reference addon for MT6789. It patches the stock GPU OPP table and CPU cpufreq-hw top entry at runtime. It is disabled until an explicit apply parameter is written.

Detailed parameters, limits, patch points, restore behavior, and known limitations are documented in [`addons/overclock_mt6789/README.md`](addons/overclock_mt6789/README.md).

## CI

`.github/workflows/build.yml` exposes the build configuration through `workflow_dispatch` inputs. Workspace caching is used only for reusable repository metadata and toolchains; the checked-out kernel worktree is recreated before source sync.

## Warning ⚠️

The addon code writes live kernel data structures and hardware registers directly. Overclocking stays off until an explicit apply parameter is written. Test changes incrementally and keep a known-good kernel available — a wrong target, incompatible kernel tree, toolchain mismatch, or vendor-driver change can cause a crash or boot failure.

### CPU frequency ceiling

CPU targets are capped at `stock_idx0 + 60%` or `2600000` KHz, whichever is lower. That's not an arbitrary safety margin: `2610000` KHz and above is confirmed to hang on this hardware.

### CFI on out-of-tree modules

Every `.ko` built against this kernel needs `__nocfi` on any function using a `kallsyms_lookup_name()`-resolved pointer, plus `-fsanitize=kcfi` in `EXTRA_CFLAGS`. A CFI violation reboots the device instantly with no panic log — there's nothing to read afterward, so get the annotation right before you flash.

## Resources

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md), build architecture and data flow
- [`docs/ADDONS.md`](docs/ADDONS.md), addon contract and build integration
- [`addons/overclock_mt6789/README.md`](addons/overclock_mt6789/README.md), module API and implementation details
- [Actions](https://github.com/Anomali1304/3-Cluster-CPU/actions/workflows/build.yml)
- [`CHANGELOG.md`](CHANGELOG.md)

## License

GPL-2.0. See [`LICENSE`](LICENSE).

## 6+1+1 virtual cpufreq policy split

`kernel/patches/apply_3_cluster_policy_split.py` patches the stock `mediatek-cpufreq-hw.c` without changing the DTB. The stock two physical hardware domains remain intact while Linux exposes three logical cpufreq policies:

| Policy | CPUs | Backing hardware domain | Table |
|---|---|---|---|
| `policy0` | CPU0-5 | stock domain 0 | stock domain-0 LUT |
| `policy6` | CPU6 | stock domain 1 | stock domain-1 LUT |
| `policy7` | CPU7 | same domain-1 registers as policy6 | same domain-1 LUT |

CPU0-5 remain one physical A55 hardware domain. CPU6 and CPU7 are separate Linux policies backed by the same physical A76 domain and LUT.

The two logical A76 policies share one physical performance-state register. Their requests are arbitrated in the patched driver and the hardware is programmed from the shared state. `get()` reports the actual shared hardware frequency.

There is no DTB modification and no third physical performance-domain node. The corresponding `overclock_mt6789` addon must be compiled against the same patched kernel tree/configuration because it mirrors the patched `struct cpufreq_mtk` layout.

## CPU voltage implementation

The `overclock_mt6789` addon can optionally follow a frequency increase with a calculated increase to the voltage field in the same CPU LUT word.

- `cpu_volt_follow=1` enables the behavior.
- `cpu_volt_max_delta_raw=3000` limits the increase to 30 mV in the documented raw-unit convention.
- The calculation uses the same physical domain's stock LUT rows 1 and 3 and rounds upward to the 625-raw EEM step.
- Only the documented LUT frequency/voltage fields are modified; other bits are preserved.

The voltage field mapping (`bits[28:12]`) is based on correlation with EEM voltage reporting and is explicitly treated as a candidate hardware-LUT voltage field, not as independently proven PMIC/MCUPM rail control. The addon README documents this limitation and the required on-device validation.
