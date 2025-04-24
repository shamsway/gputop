Okay, the core refactoring to use `_amdsmi_get_gpu_metrics` is done. The next step is to ensure the build system correctly detects and links the AMDSMI library, and then test the changes.

I've reviewed the `src/CMakeLists.txt` file. It already contains logic (lines 91-108) to:
1.  Search for `libamd_smi.so` using `find_library`.
2.  If found, define the `HAVE_AMDSMI` preprocessor macro, which activates the new code path we added.
3.  Link against the found `libamd_smi`.
4.  If not found, it prints a status message and proceeds without defining `HAVE_AMDSMI`, allowing the build to use the libdrm/sysfs fallback code.

This seems appropriate for integrating the new functionality optionally.

**Build and Test Instructions:**

1.  **Prerequisites:**
    *   Standard build tools: `cmake`, C compiler (`gcc` or `clang`), `make`.
    *   Development libraries: `ncurses` (`libncurses-dev`, `ncurses-devel`), `libdrm` (`libdrm-dev`, `libdrm-devel`), `libsystemd-dev` or `libudev-dev`.
    *   **ROCm Installation:** To test the new AMDSMI path, you **must** have the ROCm toolkit installed, as this provides `libamd_smi.so` and the necessary headers (like `amd_smi/amdsmi.h`). If ROCm is not installed, `nvtop` should still build and run using the older libdrm/sysfs method (the `#else` block).

2.  **Configure with CMake:**
    *   Navigate to your `gputop` source directory in the terminal.
    *   Create a build directory and change into it:
        ```bash
        mkdir -p build
        cd build
        ```
    *   Run CMake. Ensure AMDGPU support is enabled:
        ```bash
        cmake .. -DAMDGPU_SUPPORT=ON
        ```
    *   **Check CMake Output:** Carefully observe the output messages. You should see:
        *   `Found libamd_smi; Enabling AMD SMI support` if ROCm's `libamd_smi.so` was found.
        *   `libamd_smi not found; Falling back to libdrm/sysfs for AMDGPU support if possible.` if it was not found.
        *   If it's not found but ROCm *is* installed, you might need to help CMake find it, potentially by setting `CMAKE_PREFIX_PATH` (e.g., `cmake .. -DAMDGPU_SUPPORT=ON -DCMAKE_PREFIX_PATH=/opt/rocm`).

3.  **Compile:**
    ```bash
    make
    ```
    This should build the `nvtop` executable in the `build/src/` directory.

4.  **Test:**
    *   Run the compiled version: `./src/nvtop`
    *   **If AMDSMI was found:**
        *   Verify your AMD GPU(s) are detected. If you have partitions, check if they appear as distinct entries (e.g., "MI300A [Part 0]", "MI300A [Part 1]").
        *   Check the displayed metrics (GPU/Mem Clocks, Util%, Temp, Power, VRAM Usage, Fan%, PCIe BW). Compare them against the output of the `amd-smi` command-line tool (`/opt/rocm/bin/amd-smi` or similar) for accuracy.
        *   Look for any errors printed to the console when `nvtop` starts or runs.
        *   **Process Info:** Check if processes using the AMD GPU are listed. This depends on `amdsmi_get_gpu_process_list` working correctly, which might need further refinement.
    *   **If AMDSMI was *not* found (Fallback Test):**
        *   Verify your AMD GPU is still detected using the libdrm/sysfs method.
        *   Check if the metrics match the behavior *before* these changes.
    *   **Regression:** If you have other GPUs (NVIDIA, Intel), ensure they are still monitored correctly. Test basic UI functions (sorting, quitting).

Please proceed with building and testing using these instructions. Let me know the results, especially if CMake has trouble finding `libamd_smi` or if the displayed metrics seem incorrect when using the AMDSMI path.
