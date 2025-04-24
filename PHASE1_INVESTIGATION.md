
Okay, let's begin Phase 1: Investigation and Foundation.

**1. AMD Partitioning Deep Dive:**

I'll start by researching AMD's GPU partitioning technologies like SPX and NPSx to understand how they work and how partitions are identified.



Ran tool
Based on the search results, here's a summary of AMD GPU partitioning:

*   **Architecture:** Primarily seen on MI300 series (e.g., MI300A/X). Composed of Accelerator Complex Dies (XCDs) and I/O Dies (IODs). MI300X has 8 XCDs and 4 IODs.
*   **Compute Partitioning (MCP - Modular Chiplet Platform):**
    *   **SPX (Single Partition X-celerator):** Default mode. All 8 XCDs appear as a single logical GPU to the system (`amd-smi` shows 1 device). Workgroups are scheduled round-robin across XCDs.
    *   **CPX (Core Partitioned X-celerator):** Each XCD appears as a separate logical GPU (`amd-smi` shows 8 devices per physical MI300X). Allows explicit workgroup placement onto specific XCDs.
*   **Memory Partitioning (NPS - NUMA Per Socket):** Controls the NUMA domains exposed by the HBM memory stacks.
    *   **NPS1:** Default. Compatible with SPX and CPX. The entire HBM memory is accessible as a single pool to all XCDs.
    *   **NPS4:** Compatible *only* with CPX. Divides the memory into quadrants (pairs of HBM stacks). Each logical device (XCD) has direct, potentially higher bandwidth/lower latency access to its local quadrant, but can still access other quadrants via multi-GPU programming techniques.
*   **Identification:**
    *   In CPX mode, `amd-smi` lists each partition as a separate GPU (e.g., IDs 0-7 for the first physical MI300X, 8-15 for the second, etc.).
    *   The `amd-smi list --csv` command shows the `gpu_uuid`, which is the *same* for all partitions belonging to the same *physical* GPU. This seems like a good way to group partitions belonging to the same physical device.
    *   `amd-smi static --partition` shows the compute (CPX/SPX) and memory (NPS1/NPS4) partition mode for each listed GPU ID.
    *   Linux exposes each partition via `/dev/dri/renderD<minor_id>`. The minor IDs seem sequential for partitions within a physical GPU (e.g., `renderD128` to `renderD135` for the first MI300X).
*   **Configuration:** Partition modes (compute and memory) are set using `amd-smi set --compute-partition {CPX|SPX}` and `amd-smi set --memory-partition {NPS1|NPS4}`. Requires a reboot to take effect. Can be reset with `amd-smi reset ...`.

**2. `amd-smi` C Library (`libamd_smi`) Evaluation:**

Now, I need to investigate the C library `libamd_smi.so` (likely using the header `amdsmi.h` or `rocm_smi.h`) to see if it can provide the necessary metrics *per partition*. The search results included a link to `rocm_smi.h` documentation (version 5.6.1). While this might be slightly outdated, it's a good starting point. I'll need to search specifically for the C API documentation related to the *current* `amd-smi` version if possible, or ask you if you have access to the header files locally.

Let's search for the latest `libamd_smi` C API documentation.



Ran tool
The search results provide links to the `amdsmi` documentation on `rocm.docs.amd.com` (for ROCm 6.4.0, which seems current) and the GitHub repository.

Let's evaluate the `libamd_smi` C API based on the documentation found (primarily the `amdsmi` project docs on rocm.docs.amd.com):

*   **Header File:** The documentation refers to functions starting with `amdsmi_`, suggesting the main header is likely `amdsmi.h`. It's found in `include/amd_smi/` within the GitHub repo.
*   **Initialization:** `amdsmi_init()` exists.
*   **Shutdown:** `amdsmi_shut_down()` exists.
*   **Enumeration:**
    *   `amdsmi_get_processor_handles()`: Returns a list of handles. The documentation doesn't explicitly state if these handles represent physical GPUs *or* partitions when partitioning is enabled. However, the blog post and CLI examples strongly suggest that when CPX mode is active, each partition gets its own handle/ID. This function likely returns handles for *all* addressable compute units (physical GPUs in SPX, partitions in CPX).
    *   `amdsmi_get_socket_handles()`: Seems related to CPU sockets, not directly GPU partitions.
*   **Partition Information:**
    *   `amdsmi_get_gpu_compute_partition()`: Retrieves the current compute partition mode (CPX, SPX, etc.) for a given *handle*. This confirms handles can represent partitions.
    *   `amdsmi_get_gpu_memory_partition()`: Retrieves the current memory partition mode (NPS1, NPS4, etc.) for a given handle.
    *   **Physical GPU ID:** The documentation for `amdsmi_get_gpu_device_uuid` mentions it returns the UUID. The blog post showed that partitions of the *same* physical GPU share the *same* UUID. This seems to be the way to link a partition handle back to its physical parent device.
    *   **Partition ID within Physical GPU:** There isn't an obvious function like `amdsmi_get_partition_id()`. However, since handles are likely sequential per physical device in CPX mode (0-7, 8-15, etc.), the handle itself might implicitly contain this info relative to the handles of the same physical UUID. We might need to infer the partition index based on the list of handles sharing the same UUID.
*   **Static Info (Per Handle/Partition):**
    *   `amdsmi_get_gpu_asic_info()`: Gets market name, vendor/device/rev IDs, serial. Should work per handle.
    *   `amdsmi_get_gpu_board_info()`: Gets product serial, product name. Should work per handle.
    *   `amdsmi_get_gpu_vram_info()`: Gets VRAM type, vendor, size, width. *Crucially*, need to confirm if `vram_total` returns the partition's VRAM slice (e.g., 1/8th in CPX) or the full physical GPU VRAM. Documentation for `amdsmi_get_gpu_vram_usage` has `vram_total` and `vram_used` fields, implying per-handle totals are available. The `amdsmi_get_gpu_memory_total` function exists, but it's in the older `rocm_smi.h` doc; the newer `amdsmi.h` seems to use `amdsmi_get_gpu_vram_info`. Let's assume `amdsmi_get_gpu_vram_info` provides the per-partition VRAM size.
    *   `amdsmi_get_clock_info()`: Takes a handle and clock type (`GFX`, `MEM`, etc.) and returns *current*, *min*, and *max* frequencies. This covers clock limits.
    *   `amdsmi_get_gpu_vbios_info()`: Gets VBIOS info per handle.
*   **Dynamic Metrics (Per Handle/Partition):**
    *   `amdsmi_get_gpu_activity()`: Returns GFX, UMC (Memory), and MM (Multimedia) engine usage percentages (0-100). This looks promising for utilization metrics per partition.
    *   `amdsmi_get_temp_metric()`: Takes handle, sensor type (Edge, Junction, Memory, HBM#), and metric type (Current, Max, etc.) Returns temperature in millidegrees C. Looks good for temperature.
    *   `amdsmi_get_power_info()`: Returns *average* socket power, GFX voltage, and power limit. This gives power usage.
    *   `amdsmi_get_clock_info()`: Also returns the *current* clock frequency per handle/type.
    *   `amdsmi_get_gpu_vram_usage()`: Returns `vram_used` and `vram_total` per handle. Looks good for memory usage.
    *   `amdsmi_get_energy_count()`: Returns accumulated energy usage.
*   **Process Information (Per Handle/Partition):**
    *   `amdsmi_get_gpu_process_list()`: Takes a *handle* and returns a list of process handles running on that specific GPU/partition. **This is critical and looks like it exists!**
    *   `amdsmi_get_gpu_process_info()`: Takes a GPU/partition *handle* and a process *handle* (from the list above) and returns info including PID, name, VRAM usage (`vram_usage`), engine usage (`engine_usage` dict with `gfx_activity`, `enc_activity`, `dec_activity`). This looks like it provides the necessary per-process, per-partition resource usage.
*   **Error Handling:** Functions return an `amdsmi_status_t` enum. `amdsmi_status_code_to_string()` can convert these to strings.
*   **Thread Safety:** No explicit mention in the searched docs, but typical C libraries like this often require external locking if used across multiple threads unless otherwise specified. For `nvtop`'s current single-threaded model, this shouldn't be an issue.

**Conclusion from `libamd_smi` Evaluation:**

Based on the documentation, `libamd_smi` appears to meet almost all requirements:

*   It can enumerate handles that represent individual partitions in CPX mode.
*   It allows querying partition modes (Compute/Memory).
*   It allows identifying the parent physical GPU via shared UUIDs.
*   It provides static info (Name, VRAM size, Clocks) per handle/partition.
*   It provides dynamic metrics (Usage %, Clocks, Temp, Power, Memory Used) per handle/partition.
*   **Crucially, it seems to provide per-partition process lists and their resource usage (VRAM, engine utilization).**

**Gap:** Explicit partition ID (e.g., 0-7) within a physical GPU isn't directly returned by a dedicated function, but can likely be inferred by ordering the handles that share the same physical UUID.

**Decision:** The investigation strongly supports migrating to `libamd_smi` for AMD GPU monitoring, including partitions. The critical process information appears to be available.

**3. Current `nvtop` AMD Implementation Review:**

Now, let's examine `extract_gpuinfo_amdgpu.c` to map the currently used metrics and identify how GPUs are tracked.



Read file: src/extract_gpuinfo_amdgpu.c
Okay, reviewing the first part of `src/extract_gpuinfo_amdgpu.c`:

**Dependencies:**

*   Uses `libdrm` and `libdrm_amdgpu` directly via `dlopen`. This is what we aim to replace with `libamd_smi`.
*   Uses `<libdrm/amdgpu.h>` and `<libdrm/amdgpu_drm.h>`. These headers will be replaced by `<amd_smi/amdsmi.h>` (or whatever the final header is).
*   Uses various `sysfs` interactions (evident from file opening like `fanSpeedFILE`, `PCIeBW`, `powerCap` and functions like `readAttributeFromDevice`). `libamd_smi` should hopefully replace most, if not all, of these direct `sysfs` reads.
*   Uses `fdinfo` parsing (`extract_processinfo_fdinfo.h`, `parse_drm_fdinfo_amd`) for process information. This is the part we confirmed `libamd_smi` *should* replace with `amdsmi_get_gpu_process_list` and `amdsmi_get_gpu_process_info`.

**GPU Identification:**

*   Discovers devices using `drmGetDevices2` or `drmGetDevices`.
*   Initializes an `amdgpu_device_handle` using `_amdgpu_device_initialize` from `libdrm_amdgpu` based on the DRM file descriptor (`fd`). This handle is stored in `struct gpu_info_amdgpu`.
*   The `struct gpu_info_amdgpu` seems to be the per-GPU structure.
*   It also stores `nvtop_device` pointers (`amdgpuDevice`, `hwmonDevice`), which seem related to `sysfs` paths found via `initDeviceSysfsPaths`.
*   Uniqueness for process tracking appears to be based on a combination of a `client_id` (derived from `drmAuthMagic`), PID, and the device path string (`pdev` which is likely `/sys/class/drm/cardX`) within `struct unique_cache_id`.

**Metrics Gathered (Phase 1 Goal - Mapping):**

Let's continue reading to see exactly which metrics are populated in `gpuinfo_amdgpu_populate_static_info` and `gpuinfo_amdgpu_refresh_dynamic_info`.



Read file: src/extract_gpuinfo_amdgpu.c
**Mapping Current `nvtop` AMD Metrics to `libamd_smi` Functions:**

Here's a breakdown of the metrics gathered in the current `libdrm`/`sysfs`-based implementation (`extract_gpuinfo_amdgpu.c`) and their potential `libamd_smi` replacements:

| Metric (nvtop `gpuinfo_..._info` Field) | Current Source (`extract_gpuinfo_amdgpu.c`)                                                                                                                                | Proposed `libamd_smi` Function(s)                                                                                                                                                                   | Notes                                                                     |
| :-------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :------------------------------------------------------------------------ |
| **Static Info**                         |                                                                                                                                                                            |                                                                                                                                                                                                       |                                                                           |
| `device_name`                           | `amdgpu_get_marketing_name` (libdrm_amdgpu), `amdgpu_parse_marketing_name` (internal LUT), `drmGetVersion->desc` (libdrm)                                                     | `amdsmi_get_gpu_asic_info` -> `market_name`, `amdsmi_get_gpu_board_info` -> `product_name`                                                                                                          | Should provide a good name. Can construct a "Partition X" suffix if needed. |
| `temperature_slowdown_threshold`        | `sysfs` (`hwmon`/`temp1_crit`)                                                                                                                                             | `amdsmi_get_temp_metric` (type: `EDGE` or `JUNCTION`, metric: `CRITICAL`)                                                                                                                          | `libamd_smi` uses millidegrees C.                                         |
| `temperature_shutdown_threshold`        | `sysfs` (`hwmon`/`temp1_emergency`)                                                                                                                                        | `amdsmi_get_temp_metric` (type: `EDGE` or `JUNCTION`, metric: `EMERGENCY`)                                                                                                                         | `libamd_smi` uses millidegrees C.                                         |
| `max_pcie_link_width`                   | `nvtop_device_maximum_pcie_link` (likely uses `sysfs`)                                                                                                                     | `amdsmi_get_pcie_link_caps` -> `pcie_lanes`                                                                                                                                                           | Seems equivalent.                                                         |
| `max_pcie_gen`                          | `nvtop_device_maximum_pcie_link` -> speed converted via `nvtop_pcie_gen_from_link_speed`                                                                                     | `amdsmi_get_pcie_link_caps` -> `pcie_speed` (Convert speed GT/s to Gen)                                                                                                                               | Need conversion logic `speed -> gen`.                                     |
| `integrated_graphics`                   | `amdgpu_query_gpu_info` -> `ids_flags` & `AMDGPU_IDS_FLAGS_FUSION` (libdrm_amdgpu)                                                                                           | `amdsmi_get_gpu_asic_info` -> `vendor_id`/`device_id`? Might need a lookup table based on ID or rely on `market_name`. Or `amdsmi_get_gpu_board_info`?                                       | May require mapping device IDs or names.                                  |
| `encode_decode_shared`                  | `amdgpu_query_hw_ip_info` for `AMDGPU_HW_IP_VCN_ENC` (libdrm_amdgpu)                                                                                                        | `amdsmi_get_gpu_activity` returns `mm_activity`. May need `amdsmi_get_fw_info` or `amdsmi_get_gpu_asic_info` to check specific IP block versions if needed, but likely just use `mm_activity`. | `mm_activity` might suffice.                                              |
| **Dynamic Info**                        |                                                                                                                                                                            |                                                                                                                                                                                                       |                                                                           |
| `gpu_clock_speed`                       | `amdgpu_query_sensor_info` (`AMDGPU_INFO_SENSOR_GFX_SCLK`) (libdrm_amdgpu)                                                                                                   | `amdsmi_get_clock_info` (type: `GFX`) -> `cur_clk`                                                                                                                                                    | Units: MHz (both seem to report MHz directly or need /1000 scaling).      |
| `gpu_clock_speed_max`                   | `amdgpu_query_gpu_info` -> `max_engine_clk` (libdrm_amdgpu)                                                                                                                  | `amdsmi_get_clock_info` (type: `GFX`) -> `max_clk`                                                                                                                                                    | Units: MHz (both seem to report MHz directly or need /1000 scaling).      |
| `mem_clock_speed`                       | `amdgpu_query_sensor_info` (`AMDGPU_INFO_SENSOR_GFX_MCLK`) (libdrm_amdgpu)                                                                                                   | `amdsmi_get_clock_info` (type: `MEM`) -> `cur_clk`                                                                                                                                                    | Units: MHz (both seem to report MHz directly or need /1000 scaling).      |
| `mem_clock_speed_max`                   | `amdgpu_query_gpu_info` -> `max_memory_clk` (libdrm_amdgpu)                                                                                                                  | `amdsmi_get_clock_info` (type: `MEM`) -> `max_clk`                                                                                                                                                    | Units: MHz (both seem to report MHz directly or need /1000 scaling).      |
| `gpu_util_rate`                         | `amdgpu_query_sensor_info` (`AMDGPU_INFO_SENSOR_GPU_LOAD`) (libdrm_amdgpu)                                                                                                   | `amdsmi_get_gpu_activity` -> `gfx_activity`                                                                                                                                                           | Both report 0-100%.                                                       |
| `total_memory`                          | `amdgpu_query_info` (`AMDGPU_INFO_MEMORY`) -> `vram.total_heap_size` (libdrm_amdgpu)                                                                                         | `amdsmi_get_gpu_vram_usage` -> `vram_total`                                                                                                                                                           | Should return per-partition total.                                        |
| `used_memory`                           | `amdgpu_query_info` (`AMDGPU_INFO_MEMORY`) -> `vram.heap_usage` (libdrm_amdgpu)                                                                                              | `amdsmi_get_gpu_vram_usage` -> `vram_used`                                                                                                                                                            | Should return per-partition usage.                                        |
| `free_memory`                           | Calculated (`total_memory` - `used_memory`)                                                                                                                                  | Calculate (`vram_total` - `vram_used`)                                                                                                                                                                | Calculation remains the same.                                             |
| `mem_util_rate`                         | Calculated (`used_memory` * 100 / `total_memory`)                                                                                                                            | `amdsmi_get_gpu_activity` -> `umc_activity` OR Calculate (`vram_used` * 100 / `vram_total`)                                                                                                        | `umc_activity` might be more direct if available.                         |
| `gpu_temp`                              | `amdgpu_query_sensor_info` (`AMDGPU_INFO_SENSOR_GPU_TEMP`) (libdrm_amdgpu)                                                                                                   | `amdsmi_get_temp_metric` (type: `EDGE` or `JUNCTION`, metric: `CURRENT`)                                                                                                                          | Need to choose sensor type (Edge likely matches). Units: millidegrees C.  |
| `fan_speed`                             | `sysfs` (`hwmon`/`pwm1`) read via `fanSpeedFILE`, normalized by `maxFanValue` (`pwm1_max`)                                                                                   | `amdsmi_get_gpu_fan_speed` (returns % 0-100) OR `amdsmi_get_gpu_fan_rpms`                                                                                                                             | `amdsmi_get_gpu_fan_speed` seems more direct. Not clear if this works per partition. |
| `power_draw`                            | `amdgpu_query_sensor_info` (`AMDGPU_INFO_SENSOR_GPU_AVG_POWER`) (libdrm_amdgpu)                                                                                              | `amdsmi_get_power_info` -> `average_socket_power`                                                                                                                                                     | Units: microwatts? Needs check (`libamd_smi` docs say Watts for the struct field). |
| `pcie_link_width`                     | `nvtop_device_current_pcie_link` (likely uses `sysfs`)                                                                                                                       | `amdsmi_get_pcie_link_status` -> `pcie_lanes`                                                                                                                                                         | Seems equivalent.                                                         |
| `pcie_link_gen`                         | `nvtop_device_current_pcie_link` -> speed converted via `nvtop_pcie_gen_from_link_speed`                                                                                     | `amdsmi_get_pcie_link_status` -> `pcie_interface_version` OR `pcie_speed` (Convert speed GT/s to Gen)                                                                                                 | `pcie_interface_version` might be direct.                                 |
| `pcie_tx_throughput`                    | `sysfs` (`pcie_bw`) read via `PCIeBW`, parsed                                                                                                                                | `amdsmi_get_pcie_throughput` -> `sent` (bytes/sec?)                                                                                                                                                   | Needs unit check.                                                         |
| `pcie_rx_throughput`                    | `sysfs` (`pcie_bw`) read via `PCIeBW`, parsed                                                                                                                                | `amdsmi_get_pcie_throughput` -> `received` (bytes/sec?)                                                                                                                                               | Needs unit check.                                                         |
| `power_cap`                             | `sysfs` (`power1_cap`) read via `powerCap`                                                                                                                                   | `amdsmi_get_power_cap_info` -> `power_cap`                                                                                                                                                            | Seems equivalent.                                                         |
| `enc_util_rate`                         | Derived from `fdinfo` parsing (`drm-driver-id` and `drm-engine-*`) in `parse_drm_fdinfo_amd`                                                                                 | `amdsmi_get_gpu_activity` -> `mm_activity` OR sum `amdsmi_get_gpu_process_info` -> `engine_usage.enc_activity` across processes?                                                                 | `mm_activity` is likely average multimedia usage. Process sum might be better if `mm_activity` includes decode. Needs check. |
| `dec_util_rate`                         | Derived from `fdinfo` parsing (`drm-driver-id` and `drm-engine-*`) in `parse_drm_fdinfo_amd`                                                                                 | `amdsmi_get_gpu_activity` -> `mm_activity` OR sum `amdsmi_get_gpu_process_info` -> `engine_usage.dec_activity` across processes?                                                                 | See `enc_util_rate`.                                                      |
| **Process Info**                        |                                                                                                                                                                            |                                                                                                                                                                                                       |                                                                           |
| `processes` list                        | `get_process_list_from_fdinfo` + `parse_drm_fdinfo_amd`                                                                                                                    | `amdsmi_get_gpu_process_list`                                                                                                                                                                         | Seems equivalent.                                                         |
| Process `pid`                           | `fdinfo` parsing                                                                                                                                                           | `amdsmi_get_gpu_process_info` -> `pid`                                                                                                                                                                | Seems equivalent.                                                         |
| Process `name`                          | `get_process_name_from_pid` (standard Linux procfs)                                                                                                                          | `amdsmi_get_gpu_process_info` -> `name` OR keep using `get_process_name_from_pid`                                                                                                                     | `libamd_smi` provides it directly.                                        |
| Process `user`                          | `get_process_user_from_pid` (standard Linux procfs)                                                                                                                          | Keep using `get_process_user_from_pid`                                                                                                                                                                | `libamd_smi` doesn't seem to provide user info.                           |
| Process `gpu_mem_used`                  | `fdinfo` parsing (`drm-vram`)                                                                                                                                                | `amdsmi_get_gpu_process_info` -> `memory_usage.vram_mem`                                                                                                                                              | Seems equivalent.                                                         |
| Process `gpu_util_rate` (GFX/Compute) | `fdinfo` parsing (`drm-engine-gfx`, `drm-engine-compute`) -> converted to %                                                                                                  | `amdsmi_get_gpu_process_info` -> `engine_usage.gfx_activity` (ns). Convert ns delta over time to %.                                                                                                 | Requires calculating percentage from nanosecond counter delta.            |
| Process `enc_util_rate`                 | `fdinfo` parsing (`drm-engine-enc`) -> converted to %                                                                                                                        | `amdsmi_get_gpu_process_info` -> `engine_usage.enc_activity` (ns). Convert ns delta over time to %.                                                                                                 | Requires calculating percentage from nanosecond counter delta.            |
| Process `dec_util_rate`                 | `fdinfo` parsing (`drm-engine-dec`) -> converted to %                                                                                                                        | `amdsmi_get_gpu_process_info` -> `engine_usage.dec_activity` (ns). Convert ns delta over time to %.                                                                                                 | Requires calculating percentage from nanosecond counter delta.            |

**Mapping Summary:**

*   **Strong Matches:** Most static info (name, clocks, VRAM size), dynamic metrics (clocks, temp, power, memory usage, overall GFX/MEM util), and PCIe link status seem to have direct or near-direct equivalents in `libamd_smi`.
*   **Process Info:** `libamd_smi` appears to provide the necessary process list and per-process VRAM usage. Per-process engine usage is provided in nanoseconds (`engine_usage`), requiring `nvtop` to calculate the percentage based on the time delta between updates, similar to how it currently calculates fdinfo-based percentages.
*   **Potential Minor Gaps/Changes:**
    *   Fan speed reporting might need verification for partition behavior.
    *   Identifying integrated graphics might require a lookup table or name parsing.
    *   PCIe throughput units need verification.
    *   Exact mapping for ENC/DEC utilization (whether `mm_activity` is sufficient or process summation is needed) requires clarification, but the process-level data *is* available.
    *   User info for processes still needs to come from standard Linux `procfs`.

**Overall Assessment:** Phase 1 investigation is complete. The results strongly favor migrating to `libamd_smi`. It appears capable of providing all necessary static, dynamic, and per-partition process metrics currently used by `nvtop`, replacing the mix of `libdrm`, `sysfs`, and `fdinfo` parsing.

Phase 1 is complete. Ready to move on to Phase 2: Design.
