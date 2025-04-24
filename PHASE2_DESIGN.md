
Excellent. Phase 1 is complete, and it confirms that `libamd_smi` is a suitable replacement and provides the necessary data, including per-partition process information.

Let's proceed to **Phase 2: Design**.

**1. Data Acquisition Strategy:**

*   **Commitment:** We will fully migrate AMD GPU monitoring to use the `libamd_smi` C library, replacing the current `libdrm`/`sysfs`/`fdinfo` approach in `extract_gpuinfo_amdgpu.c`.
*   **Partition Representation:** Each `amdsmi_processor_handle` returned by `libamd_smi` (representing a physical GPU in SPX mode or a partition in CPX mode) will be treated as a distinct `gpu_info` instance within `nvtop`.
*   **Handle Mapping & Identification:**
    *   The `amdsmi_processor_handle` will be stored within the `gpu_info` structure (likely within a new `struct gpu_info_amdsmi` that embeds `struct gpu_info`).
    *   During device discovery (`gpuinfo_amdsmi_get_device_handles`):
        *   Retrieve all processor handles using `amdsmi_get_processor_handles`.
        *   For each handle, query its physical GPU UUID using `amdsmi_get_gpu_device_uuid` and store it.
        *   Group the handles based on their shared physical UUID.
        *   Within each group (representing one physical GPU), assign a sequential `partition_index` (0, 1, 2, ...) based on the order the handles were returned or sorted by handle value/BDF for consistency. Store this index.
*   **Data Structures:**
    *   We'll define `struct gpu_info_amdsmi` containing `struct gpu_info base;` and add fields for `amdsmi_processor_handle processor_handle;`, `char physical_gpu_uuid[UUID_STRING_LENGTH];`, and `unsigned partition_index;`.
    *   The existing `gpuinfo_static_info` and `gpuinfo_dynamic_info` structures within `struct gpu_info` appear suitable for holding the metrics retrieved from `libamd_smi` functions.
    *   The device name in `static_info` will be populated using `amdsmi_get_gpu_asic_info` (`market_name`) or `amdsmi_get_gpu_board_info` (`product_name`) and appended with "[GPU PhysID / Part PartIdx]" if partitioning is detected (e.g., "MI300X [UUID_Short / Part 1]"). We'll need a way to get a short identifier for the physical GPU, perhaps derived from the UUID or by assigning a simple physical index (0, 1, 2...) based on UUID discovery order.

**2. UI/UX Design for Many GPUs:**

*   **Approach:** Implement **Pagination** as the primary mechanism to handle a large number of GPU/partition entries.
*   **Layout & Logic:**
    *   Calculate `gpus_per_page` based on available terminal height and the space required per GPU widget in `interface_setup_win.c`.
    *   Calculate `total_pages = ceil(total_discovered_handles / gpus_per_page)`.
    *   Maintain a `current_page` index (0-based).
    *   Display a "Page X / Y" indicator (e.g., in the header or footer area managed by `interface.c`).
*   **Keybindings:** Implement the following in `interface.c` (`handle_input`):
    *   `PgDn`: Increment `current_page`, wrap to 0 if `current_page >= total_pages`.
    *   `PgUp`: Decrement `current_page`, wrap to `total_pages - 1` if `current_page < 0`.
    *   `Home`: Set `current_page = 0`.
    *   `End`: Set `current_page = total_pages - 1`.
*   **Rendering:**
    *   Modify the GPU widget drawing loops in `interface.c` (e.g., `draw_gpu_widgets`, `draw_process_widgets` if applicable) to render only the GPUs within the range `[current_page * gpus_per_page, min((current_page + 1) * gpus_per_page, total_discovered_handles))`.
    *   Ensure process display corresponds to the *selected* GPU, which might only be one of the GPUs visible on the current page. The selection mechanism might need minor adjustments if it relies on the absolute index in the full list.

**3. Configuration:**

*   **GPUs per page:** Will be calculated automatically based on terminal height. No user configuration initially.
*   **Grouping/Sorting:**
    *   The global list of `gpu_info` structures will be sorted *before* pagination is applied.
    *   The primary sort key will be the `physical_gpu_uuid`.
    *   The secondary sort key will be the `partition_index`.
    *   This ensures partitions from the same physical device are displayed contiguously across pages.

This design addresses the core requirements for `libamd_smi` integration and handling numerous logical GPUs via pagination and sorting. We are ready to proceed to Phase 3: Implementation.
