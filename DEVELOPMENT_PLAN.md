
Okay, this is an exciting enhancement for `nvtop`, especially given the rise of GPU partitioning on high-end AMD Instinct hardware. Handling many logical GPUs presents both a data acquisition challenge and a UI challenge.

Here is a detailed development plan to add AMD GPU partitioning support and enhance `nvtop`'s display for numerous GPUs, potentially leveraging the `amd-smi` library:

**Goal:** Enhance `nvtop` to accurately monitor individual AMD GPU partitions (e.g., on Instinct MI300A/X) and provide a usable interface for systems with a large number of logical GPUs (physical GPUs * partitions per GPU).

**Potential Core Technology Shift:** Migrate AMD GPU monitoring from `libdrm`/`sysfs` to the `amd-smi` C library (`libamd_smi.so`), as it likely has native support for querying partition information and metrics.

**Development Plan:**

**Phase 1: Investigation & Foundation (Prerequisite: Access to `amd-smi` C library documentation/headers)**

1.  **AMD Partitioning Deep Dive:**
    *   Research AMD GPU partitioning modes (e.g., SPX, NPSx) and how they are configured and exposed by the ROCm stack.
    *   Understand how partitions are identified (unique IDs, relation to physical device).
2.  **`amd-smi` C Library (`libamd_smi`) Evaluation:**
    *   **Goal:** Confirm `libamd_smi` can provide all necessary data points currently used by `nvtop` for AMD, *specifically for individual partitions*.
    *   **Tasks:**
        *   Locate the `amd_smi/amdsmi.h` header file and relevant library documentation.
        *   Identify functions for:
            *   Initializing the library (`amdsmi_init`).
            *   Enumerating *all* processor handles, including logical partitions (`amdsmi_get_processor_handles` or similar).
            *   Querying partition information (parent physical GPU ID, partition type, partition ID). (Look for functions like `amdsmi_get_partition_info` if they exist).
            *   Querying static info per partition (Name, VRAM size, Clocks, etc.) (`amdsmi_get_gpu_board_info`, `amdsmi_get_gpu_metric` for static values).
            *   Querying dynamic metrics per partition (Usage %, Clocks, Temp, Power, Memory Used) (`amdsmi_get_temp_metric`, `amdsmi_get_gpu_metric` for dynamic values like utilization, power, clocks, memory).
            *   Querying process information *per partition*. Does `amd-smi` provide a list of processes running on a specific partition and their resource usage (VRAM, engine utilization)? This is critical and might be the hardest part to replace if `amd-smi` doesn't expose it directly. (Look for functions like `amdsmi_get_process_list` or `amdsmi_get_process_info` and check if they can be filtered by partition handle).
        *   Determine error handling mechanisms within the library.
        *   Assess thread safety if multi-threading is planned (unlikely for current `nvtop` structure but good to know).
3.  **Current `nvtop` AMD Implementation Review:**
    *   Map existing metrics gathered in `extract_gpuinfo_amdgpu.c` (via `libdrm`, `sysfs`, `fdinfo`) to potential `libamd_smi` functions identified above. Identify any potential gaps.
    *   Understand how `nvtop` currently uniquely identifies GPUs (`pdev` string, file descriptors). This will need to adapt to partition handles.

**Phase 2: Design**

1.  **Data Acquisition Strategy:**
    *   **Decision:** Commit to using `libamd_smi` fully for AMD GPUs, or use a hybrid approach if process info is unavailable? (Assume full switch for now, adapt if needed).
    *   Define how `nvtop` will discover and represent partitions. Each partition should likely become a distinct `gpu_info` entry.
    *   Design mapping from `amdsmi_processor_handle` (for partitions) to `nvtop`'s internal GPU representation. Include storing the physical device ID alongside the partition ID.
    *   Define data structures within `struct gpu_info` (or a new derived struct for `amd-smi`) to hold partition-specific info and metrics obtained from `libamd_smi`.
2.  **UI/UX Design for Many GPUs:**
    *   **Requirement:** The UI must be usable with potentially 64+ GPU entries.
    *   **Options:**
        *   **Pagination:** Display a fixed number of GPUs per screen (e.g., 8 or 10) with keybindings (e.g., PgUp/PgDn, Arrow Keys) to navigate between pages. Show "Page X of Y" indicator.
        *   **Summary View:** Create a new compact display mode showing only essential metrics (e.g., Name, Util%, Mem%, Temp, Power) for many GPUs on one screen. Allow toggling between summary and the standard detailed view (perhaps only for the *selected* GPU in summary mode, or via pagination in summary mode).
        *   **Filtering/Sorting:** Allow filtering (e.g., show only partitions of physical GPU X) or sorting (e.g., by utilization). (More complex, maybe a follow-on).
    *   **Decision:** Start with Pagination as it's generally more straightforward to implement than a completely new view. Design the layout and keybindings.
3.  **Configuration:**
    *   Decide if the number of GPUs per page should be configurable (e.g., in `nvtoprc`).
    *   Consider a configuration option to group partitions by physical GPU in the display.

**Phase 3: Implementation**

1.  **Backend (`extract_gpuinfo_amdgpu.c` Refactor / New `extract_gpuinfo_amdsmi.c`):**
    *   Add CMake logic to find and link `libamd_smi.so`. Make it optional if `nvtop` should still build without it (falling back to old method or disabling AMD support).
    *   Replace dynamic loading of `libdrm`/`libdrm_amdgpu` with loading `libamd_smi`.
    *   Implement `gpuinfo_amdsmi_init` using `amdsmi_init`.
    *   Implement `gpuinfo_amdsmi_get_device_handles`:
        *   Use `amdsmi_get_processor_handles` (or equivalent) to get handles for *all* AMD GPUs/partitions.
        *   Create a `gpu_info` structure for *each* partition handle. Store the handle.
        *   Query basic static info (like partition ID, physical GPU ID) needed for identification.
    *   Implement `gpuinfo_amdsmi_populate_static_info`:
        *   Use `libamd_smi` functions to query VRAM size, name (potentially construct like "MI300A [GPU 0 / Part 1]"), clock limits, etc., for the given partition handle.
    *   Implement `gpuinfo_amdsmi_refresh_dynamic_info`:
        *   Use `libamd_smi` functions to get current temp, power, clocks, utilization (GFX, MEM), VRAM used for the partition handle.
    *   Implement `gpuinfo_amdsmi_get_running_processes`:
        *   **Attempt 1:** Use `libamd_smi` process query functions if available and filter by partition handle.
        *   **Attempt 2 (If Attempt 1 fails):** Investigate if the existing `fdinfo` parsing method (`extract_processinfo_fdinfo.c`) can be adapted. This might be difficult if `fdinfo` doesn't distinguish partitions. This is the biggest risk area.
        *   **Fallback:** Initially, process info might be unavailable for AMD partitions if neither library function nor `fdinfo` works easily. Document this limitation.
    *   Update error handling using `libamd_smi` status codes.
    *   Register the new `amd-smi` backend (`gpu_vendor_amdsmi`) and potentially disable/remove the old `amdgpu` one.
2.  **Frontend (UI - `interface.c`, `interface_setup_win.c`, etc.):**
    *   Modify `setup_gpu_widgets` and related drawing functions (`draw_gpu_widgets`, `draw_process_widgets`) to handle a potentially large list of `gpu_info` structures.
    *   Implement pagination logic:
        *   Calculate the number of pages based on available vertical space and the total number of detected partitions.
        *   Store the current page index.
        *   Modify drawing loops to only render GPUs belonging to the current page.
        *   Add keybindings (e.g., in `handle_input`) for PgUp/PgDn to change the page index.
        *   Draw a page indicator (e.g., "Page 2/8") somewhere on the screen.
    *   Adjust layout calculations to account for the pagination display.
    *   Update the help screen (`F1`) with new keybindings.

**Phase 4: Testing**

1.  **Environment:** Requires a system with an AMD Instinct GPU where partitioning can be enabled/disabled. Access to multi-GPU systems is ideal.
2.  **Unit/Integration Tests:**
    *   Test device discovery with partitioning enabled (various partition counts) and disabled.
    *   Verify metric accuracy for each partition against `amd-smi` CLI tool output.
    *   Test process tracking per partition (if implemented).
    *   Test on systems with only NVIDIA or Intel GPUs to ensure no regressions.
    *   Test build with and without `libamd_smi` installed (if optional linking is implemented).
3.  **UI Tests:**
    *   Test pagination navigation with varying numbers of GPUs (less than one page, exactly one page, multiple pages).
    *   Test UI responsiveness with many GPUs.
    *   Test terminal resizing behavior.
    *   Test different color schemes.
4.  **Real-world Testing:** Run `nvtop` during workloads known to stress specific partitions and verify the display reflects the activity accurately.

**Phase 5: Documentation & Release**

1.  Update `README.md`:
    *   Explain the new AMD partition support.
    *   Document the dependency on `libamd_smi` (part of ROCm).
    *   Update build instructions if necessary.
    *   Explain the pagination feature and keybindings.
2.  Update `nvtop.1` man page.
3.  Add entries to `CHANGELOG.md`.
4.  Prepare and tag a new release.

**Potential Challenges:**

*   **Process Info via `libamd_smi`:** As mentioned, getting reliable per-partition process information might be difficult if the library doesn't expose it clearly.
*   **`libamd_smi` API Stability:** Ensure usage aligns with a reasonably stable version of the library.
*   **Performance:** Querying metrics for a very large number of partitions (e.g., 64+) frequently might introduce performance overhead. Monitor this during testing.
*   **Error Handling:** Robustly handling cases where `libamd_smi` fails or provides unexpected data.

This plan provides a structured approach. The first critical step is the deep investigation into `libamd_smi`'s capabilities regarding partitioning and process monitoring.
