/*
 * Copyright (C) 2012 Lauri Kasanen
 * Copyright (C) 2018 Genesis Cloud Ltd.
 * Copyright (C) 2022 YiFei Zhu <zhuyifei1999@gmail.com>
 * Copyright (C) 2022 Maxime Schmitt <maxime.schmitt91@gmail.com>
 * Copyright (C) 2023 Advanced Micro Devices, Inc. All rights reserved.
 *
 * This file is part of Nvtop and adapted from radeontop.
 *
 * Nvtop is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nvtop is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nvtop.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "nvtop/common.h"
#include "nvtop/device_discovery.h"
#include "nvtop/extract_gpuinfo_common.h"
#include "nvtop/extract_processinfo_fdinfo.h"
#include "nvtop/time.h"

#include <assert.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
// Conditional include based on CMake detection
#ifdef HAVE_AMDSMI
#include <amd_smi/amdsmi.h>
#else
// Only include these if AMDSMI is NOT available (fallback)
#include <libdrm/amdgpu.h>
#include <libdrm/amdgpu_drm.h>
#include <xf86drm.h>
#endif
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#include <uthash.h>
#include <uuid/uuid.h> // For UUID handling

// extern
#ifndef HAVE_AMDSMI
// Only declare if AMDSMI is NOT available (fallback)
const char *amdgpu_parse_marketing_name(struct amdgpu_gpu_info *info);
#endif

#ifdef HAVE_AMDSMI
// Local function pointers to AMD SMI library
static typeof(amdsmi_init) *_amdsmi_init;
static typeof(amdsmi_shut_down) *_amdsmi_shut_down;
static typeof(amdsmi_get_processor_handles) *_amdsmi_get_processor_handles;
static typeof(amdsmi_get_processor_type) *_amdsmi_get_processor_type;
static typeof(amdsmi_get_gpu_device_uuid) *_amdsmi_get_gpu_device_uuid;
static typeof(amdsmi_get_gpu_device_bdf) *_amdsmi_get_gpu_device_bdf;
static typeof(amdsmi_get_gpu_compute_partition) *_amdsmi_get_gpu_compute_partition;
static typeof(amdsmi_get_gpu_memory_partition) *_amdsmi_get_gpu_memory_partition;
static typeof(amdsmi_get_gpu_asic_info) *_amdsmi_get_gpu_asic_info;
static typeof(amdsmi_get_gpu_board_info) *_amdsmi_get_gpu_board_info;
static typeof(amdsmi_get_gpu_vram_info) *_amdsmi_get_gpu_vram_info;
static typeof(amdsmi_get_gpu_vram_usage) *_amdsmi_get_gpu_vram_usage;
static typeof(amdsmi_get_clock_info) *_amdsmi_get_clock_info;
static typeof(amdsmi_get_temp_metric) *_amdsmi_get_temp_metric;
static typeof(amdsmi_get_gpu_fan_speed) *_amdsmi_get_gpu_fan_speed;
static typeof(amdsmi_get_power_info) *_amdsmi_get_power_info;
static typeof(amdsmi_get_power_cap_info) *_amdsmi_get_power_cap_info;
static typeof(amdsmi_get_pcie_link_status) *_amdsmi_get_pcie_link_status;
static typeof(amdsmi_get_pcie_link_caps) *_amdsmi_get_pcie_link_caps;
static typeof(amdsmi_get_pcie_throughput) *_amdsmi_get_pcie_throughput;
static typeof(amdsmi_get_gpu_activity) *_amdsmi_get_gpu_activity;
static typeof(amdsmi_get_gpu_process_list) *_amdsmi_get_gpu_process_list;
static typeof(amdsmi_get_gpu_process_info) *_amdsmi_get_gpu_process_info;
static typeof(amdsmi_status_code_to_string) *_amdsmi_status_code_to_string;
static typeof(amdsmi_get_gpu_metrics) *_amdsmi_get_gpu_metrics;
static typeof(amdsmi_get_pcie_bandwidth) *_amdsmi_get_pcie_bandwidth;
// Add other needed amdsmi function pointers here...

static void *libamdsmi_handle;

#else // Fallback to libdrm/amdgpu definitions

// Local function pointers to DRM interface
static typeof(drmGetDevices) *_drmGetDevices;
static typeof(drmGetDevices2) *_drmGetDevices2;
static typeof(drmFreeDevices) *_drmFreeDevices;
static typeof(drmGetVersion) *_drmGetVersion;
static typeof(drmFreeVersion) *_drmFreeVersion;
static typeof(drmGetMagic) *_drmGetMagic;
static typeof(drmAuthMagic) *_drmAuthMagic;
static typeof(drmDropMaster) *_drmDropMaster;

// Local function pointers to amdgpu DRM interface
static typeof(amdgpu_device_initialize) *_amdgpu_device_initialize;
static typeof(amdgpu_device_deinitialize) *_amdgpu_device_deinitialize;
static typeof(amdgpu_get_marketing_name) *_amdgpu_get_marketing_name;
static typeof(amdgpu_query_hw_ip_info) *_amdgpu_query_hw_ip_info;
static typeof(amdgpu_query_gpu_info) *_amdgpu_query_gpu_info;
static typeof(amdgpu_query_info) *_amdgpu_query_info;
static typeof(amdgpu_query_sensor_info) *_amdgpu_query_sensor_info;

static void *libdrm_handle;
static void *libdrm_amdgpu_handle;

static int last_libdrm_return_status = 0;
#endif

static char didnt_call_gpuinfo_init[] = "uninitialized";
static const char *local_error_string = didnt_call_gpuinfo_init;

// Process cache structures remain the same for now, but keys might change
// if we don't rely on DRM client_id anymore.
// Key structure for uthash (pid + pdev)
struct unique_cache_id {
  pid_t pid;
  char *pdev; // pdev string (e.g., "0000:01:00.0")
  // Add other fields if needed for uniqueness across partitions, e.g., processor handle?
};

// Macro to find cache entry by pid and pdev
#define HASH_FIND_PID_PDEV(head, pid_key, pdev_key, out_ptr)                                                            \\\
  HASH_FIND(hh, head, &(struct unique_cache_id){.pid = pid_key, .pdev = pdev_key}, sizeof(struct unique_cache_id), out_ptr)

// Macro to add cache entry (key is derived from client_id member within the struct)
#define HASH_ADD_PID_PDEV(head, in_ptr) HASH_ADD(hh, head, client_id, sizeof(struct unique_cache_id), in_ptr)

// Structure to hold unique identifier for process cache (using pid + pdev)
// struct __attribute__((__packed__)) unique_cache_id { // Packed attribute removed, let compiler align
//   // unsigned client_id; // No longer needed from DRM
//   pid_t pid;
//   char *pdev; // Keep pdev for now, or maybe use handle?
//   // Consider adding processor handle if needed for uniqueness across partitions
// }; // Definition moved above macros

// Process info cache needs adjustment for AMDSMI usage data
struct amdsmi_process_info_cache {
  struct unique_cache_id client_id; // Key for the hash table (contains pid and pdev)
  // Store engine usage in nanoseconds from AMDSMI
  uint64_t gfx_engine_ns;
  uint64_t compute_engine_ns; // AMDSMI might not separate compute explicitly
  uint64_t enc_engine_ns;
  uint64_t dec_engine_ns;
  // Add other engine types if provided by AMDSMI (e.g., mm_engine_ns?)
  nvtop_time last_measurement_tstamp;
  // Validation bits might need adjustment based on available AMDSMI process data
#define AMDSMI_CACHE_FIELDS 4 // gfx, compute, enc, dec
  unsigned char valid[(AMDSMI_CACHE_FIELDS + CHAR_BIT - 1) / CHAR_BIT];
  UT_hash_handle hh;
};

// Define validation bits enum (mirroring struct order)
enum amdsmi_cache_info_valid {
  amdsmi_cache_gfx_engine_ns_valid = 0,
  amdsmi_cache_compute_engine_ns_valid,
  amdsmi_cache_enc_engine_ns_valid,
  amdsmi_cache_dec_engine_ns_valid,
  // Add others if needed
};

// Macros for setting/checking validity in the cache
#define SET_AMDSMI_CACHE(structPtr, field, value)                                                                      \\\
  do {                                                                                                                 \\\
    (structPtr)->field = (value);                                                                                      \\\
    SET_VALID(amdsmi_cache_##field##_valid, (structPtr)->valid);                                                       \\\
  } while (0)
#define AMDSMI_CACHE_FIELD_VALID(structPtr, field) VALUE_IS_VALID(structPtr, field, amdsmi_cache_)

// Define the main struct for AMDSMI
struct gpu_info_amdsmi {
  struct gpu_info base;

#ifdef HAVE_AMDSMI
  amdsmi_processor_handle processor_handle;
  char physical_gpu_uuid_str[37]; // Standard UUID string length + null terminator
  unsigned partition_index;
  // No longer need drmVersion, fd, amdgpu_device_handle, sysfs FILE*
#else
  // Keep existing libdrm fields for fallback
  drmVersionPtr drmVersion;
  int fd;
  amdgpu_device_handle amdgpu_device;
  FILE *fanSpeedFILE;
  FILE *PCIeBW;
  FILE *powerCap;
  nvtop_device *amdgpuDevice;
  nvtop_device *hwmonDevice;
#endif

  // Adjust process cache type and maybe name
  struct amdsmi_process_info_cache *last_update_process_cache, *current_update_process_cache;

#ifndef HAVE_AMDSMI // Only needed for libdrm fallback
  unsigned maxFanValue;
#endif
};

unsigned amdsmi_count; // Renamed from amdgpu_count
static struct gpu_info_amdsmi *gpu_infos; // Renamed

// Function declarations need renaming and potentially signature changes
static bool gpuinfo_amdsmi_init(void);
static void gpuinfo_amdsmi_shutdown(void);
static const char *gpuinfo_amdsmi_last_error_string(void);
static bool gpuinfo_amdsmi_get_device_handles(struct list_head *devices, unsigned *count);
static void gpuinfo_amdsmi_populate_static_info(struct gpu_info *_gpu_info);
static void gpuinfo_amdsmi_refresh_dynamic_info(struct gpu_info *_gpu_info);
static void gpuinfo_amdsmi_get_running_processes(struct gpu_info *_gpu_info);

// Update the vendor struct registration
struct gpu_vendor gpu_vendor_amdsmi = {
    .init = gpuinfo_amdsmi_init,
    .shutdown = gpuinfo_amdsmi_shutdown,
    .last_error_string = gpuinfo_amdsmi_last_error_string,
    .get_device_handles = gpuinfo_amdsmi_get_device_handles,
    .populate_static_info = gpuinfo_amdsmi_populate_static_info,
    .refresh_dynamic_info = gpuinfo_amdsmi_refresh_dynamic_info,
    .refresh_running_processes = gpuinfo_amdsmi_get_running_processes,
    .name = "AMD", // Keep name as AMD
};

// Need to remove or #ifdef out functions relying solely on DRM/sysfs
#ifndef HAVE_AMDSMI
static int readAttributeFromDevice(nvtop_device *dev, const char *sysAttr, const char *format, ...);
#endif

__attribute__((constructor)) static void init_extract_gpuinfo_amdsmi(void) {
#ifdef AMDGPU_SUPPORT // Keep CMake build flag guard
  register_gpu_vendor(&gpu_vendor_amdsmi);
#endif
}

#ifndef HAVE_AMDSMI
// Keep libdrm-specific helpers only for fallback
static int wrap_drmGetDevices(drmDevicePtr devices[], int max_devices) {
  assert(_drmGetDevices2 || _drmGetDevices);

  if (_drmGetDevices2)
    return _drmGetDevices2(0, devices, max_devices);
  return _drmGetDevices(devices, max_devices);
}

static bool parse_drm_fdinfo_amd(struct gpu_info *info, FILE *fdinfo_file, struct gpu_process *process_info);
#endif

static bool gpuinfo_amdsmi_init(void) {
#ifdef HAVE_AMDSMI
  libamdsmi_handle = dlopen("libamd_smi.so", RTLD_LAZY);
  // Try versioned names if the base name fails
  if (!libamdsmi_handle)
    libamdsmi_handle = dlopen("libamd_smi.so.1", RTLD_LAZY); // Example version

  if (!libamdsmi_handle) {
    local_error_string = dlerror();
    return false;
  }

  // Load all required amdsmi functions using dlsym
  _amdsmi_init = dlsym(libamdsmi_handle, "amdsmi_init");
  _amdsmi_shut_down = dlsym(libamdsmi_handle, "amdsmi_shut_down");
  _amdsmi_get_processor_handles = dlsym(libamdsmi_handle, "amdsmi_get_processor_handles");
  _amdsmi_get_processor_type = dlsym(libamdsmi_handle, "amdsmi_get_processor_type");
  _amdsmi_get_gpu_device_uuid = dlsym(libamdsmi_handle, "amdsmi_get_gpu_device_uuid");
  _amdsmi_get_gpu_device_bdf = dlsym(libamdsmi_handle, "amdsmi_get_gpu_device_bdf");
  _amdsmi_get_gpu_compute_partition = dlsym(libamdsmi_handle, "amdsmi_get_gpu_compute_partition");
  _amdsmi_get_gpu_memory_partition = dlsym(libamdsmi_handle, "amdsmi_get_gpu_memory_partition");
  _amdsmi_get_gpu_asic_info = dlsym(libamdsmi_handle, "amdsmi_get_gpu_asic_info");
  _amdsmi_get_gpu_board_info = dlsym(libamdsmi_handle, "amdsmi_get_gpu_board_info");
  _amdsmi_get_gpu_vram_info = dlsym(libamdsmi_handle, "amdsmi_get_gpu_vram_info");
  _amdsmi_get_gpu_vram_usage = dlsym(libamdsmi_handle, "amdsmi_get_gpu_vram_usage");
  _amdsmi_get_clock_info = dlsym(libamdsmi_handle, "amdsmi_get_clock_info");
  _amdsmi_get_temp_metric = dlsym(libamdsmi_handle, "amdsmi_get_temp_metric");
  _amdsmi_get_gpu_fan_speed = dlsym(libamdsmi_handle, "amdsmi_get_gpu_fan_speed");
  _amdsmi_get_power_info = dlsym(libamdsmi_handle, "amdsmi_get_power_info");
  _amdsmi_get_power_cap_info = dlsym(libamdsmi_handle, "amdsmi_get_power_cap_info");
  _amdsmi_get_pcie_link_status = dlsym(libamdsmi_handle, "amdsmi_get_pcie_link_status");
  _amdsmi_get_pcie_link_caps = dlsym(libamdsmi_handle, "amdsmi_get_pcie_link_caps");
  _amdsmi_get_pcie_throughput = dlsym(libamdsmi_handle, "amdsmi_get_pcie_throughput");
  _amdsmi_get_gpu_activity = dlsym(libamdsmi_handle, "amdsmi_get_gpu_activity");
  _amdsmi_get_gpu_process_list = dlsym(libamdsmi_handle, "amdsmi_get_gpu_process_list");
  _amdsmi_get_gpu_process_info = dlsym(libamdsmi_handle, "amdsmi_get_gpu_process_info");
  _amdsmi_status_code_to_string = dlsym(libamdsmi_handle, "amdsmi_status_code_to_string");
  _amdsmi_get_gpu_metrics = dlsym(libamdsmi_handle, "amdsmi_get_gpu_metrics");
  _amdsmi_get_pcie_bandwidth = dlsym(libamdsmi_handle, "amdsmi_get_pcie_bandwidth");

  // Check essential functions needed for basic operation and info gathering
  if (!_amdsmi_init || !_amdsmi_shut_down || !_amdsmi_get_processor_handles ||
      !_amdsmi_get_processor_type || !_amdsmi_get_gpu_device_uuid || !_amdsmi_get_gpu_device_bdf ||
      !_amdsmi_status_code_to_string ||
      // Static info functions
      !_amdsmi_get_gpu_asic_info || !_amdsmi_get_gpu_board_info || !_amdsmi_get_gpu_vram_info ||
      !_amdsmi_get_clock_info || !_amdsmi_get_temp_metric || !_amdsmi_get_pcie_link_caps ||
      // Dynamic info functions (for refresh)
      !_amdsmi_get_gpu_metrics || !_amdsmi_get_gpu_vram_usage || !_amdsmi_get_pcie_bandwidth ||
      // Optional but useful static info
      !_amdsmi_get_gpu_fan_speed || !_amdsmi_get_power_info || !_amdsmi_get_power_cap_info ||
      !_amdsmi_get_pcie_link_status ||
      // Process list functions (potentially optional depending on usage)
      !_amdsmi_get_gpu_process_list || !_amdsmi_get_gpu_process_info
      // !_amdsmi_get_gpu_activity // Keep activity optional for now
     ) {
    local_error_string = "Failed to load required AMD SMI functions";
    // Potentially use _amdsmi_status_code_to_string(ret, &local_error_string) if safe after failed init
    dlclose(libamdsmi_handle);
    libamdsmi_handle = NULL;
    return false;
  }

  // Initialize the AMD SMI library
  amdsmi_status_t ret = _amdsmi_init(0); // Flags = 0 for now
  if (ret != AMDSMI_STATUS_SUCCESS) {
    // Use status_code_to_string? Need to load it first even if init fails...
    local_error_string = "amdsmi_init failed"; // Placeholder
    // Potentially use _amdsmi_status_code_to_string(ret, &local_error_string) if safe after failed init
    dlclose(libamdsmi_handle);
    libamdsmi_handle = NULL;
    return false;
  }

#else // Fallback to libdrm initialization
  libdrm_handle = dlopen("libdrm.so", RTLD_LAZY);
  if (!libdrm_handle)
    libdrm_handle = dlopen("libdrm.so.2", RTLD_LAZY);
  if (!libdrm_handle)
    libdrm_handle = dlopen("libdrm.so.1", RTLD_LAZY);
  if (!libdrm_handle) {
    local_error_string = dlerror();
    return false;
  }

  _drmGetDevices2 = dlsym(libdrm_handle, "drmGetDevices2");
  if (!_drmGetDevices2)
    _drmGetDevices = dlsym(libdrm_handle, "drmGetDevices");
  if (!_drmGetDevices2 && !_drmGetDevices)
    goto init_error_clean_exit;

  _drmFreeDevices = dlsym(libdrm_handle, "drmFreeDevices");
  if (!_drmFreeDevices)
    goto init_error_clean_exit;

  _drmGetVersion = dlsym(libdrm_handle, "drmGetVersion");
  if (!_drmGetVersion)
    goto init_error_clean_exit;

  _drmFreeVersion = dlsym(libdrm_handle, "drmFreeVersion");
  if (!_drmFreeVersion)
    goto init_error_clean_exit;

  _drmGetMagic = dlsym(libdrm_handle, "drmGetMagic");
  if (!_drmGetMagic)
    goto init_error_clean_exit;

  _drmAuthMagic = dlsym(libdrm_handle, "drmAuthMagic");
  if (!_drmAuthMagic)
    goto init_error_clean_exit;

  _drmDropMaster = dlsym(libdrm_handle, "drmDropMaster");
  if (!_drmDropMaster)
    goto init_error_clean_exit;

  libdrm_amdgpu_handle = dlopen("libdrm_amdgpu.so", RTLD_LAZY);
  if (!libdrm_amdgpu_handle)
    libdrm_amdgpu_handle = dlopen("libdrm_amdgpu.so.1", RTLD_LAZY);

  if (libdrm_amdgpu_handle) {
    _amdgpu_device_initialize = dlsym(libdrm_amdgpu_handle, "amdgpu_device_initialize");
    _amdgpu_device_deinitialize = dlsym(libdrm_amdgpu_handle, "amdgpu_device_deinitialize");
    _amdgpu_get_marketing_name = dlsym(libdrm_amdgpu_handle, "amdgpu_get_marketing_name");
    _amdgpu_query_hw_ip_info = dlsym(libdrm_amdgpu_handle, "amdgpu_query_hw_ip_info");
    _amdgpu_query_info = dlsym(libdrm_amdgpu_handle, "amdgpu_query_info");
    _amdgpu_query_gpu_info = dlsym(libdrm_amdgpu_handle, "amdgpu_query_gpu_info");
    _amdgpu_query_sensor_info = dlsym(libdrm_amdgpu_handle, "amdgpu_query_sensor_info");
  }

  local_error_string = NULL;
  return true;

init_error_clean_exit:
  dlclose(libdrm_handle);
  libdrm_handle = NULL;
  return false;
#endif
}

static void gpuinfo_amdsmi_shutdown(void) {
#ifdef HAVE_AMDSMI
  for (unsigned i = 0; i < amdsmi_count; ++i) {
    struct gpu_info_amdsmi *gpu_info = &gpu_infos[i];
    if (gpu_info->fanSpeedFILE)
      fclose(gpu_info->fanSpeedFILE);
    if (gpu_info->PCIeBW)
      fclose(gpu_info->PCIeBW);
    if (gpu_info->powerCap)
      fclose(gpu_info->powerCap);
    nvtop_device_unref(gpu_info->amdgpuDevice);
    nvtop_device_unref(gpu_info->hwmonDevice);
    _drmFreeVersion(gpu_info->drmVersion);
    _amdgpu_device_deinitialize(gpu_info->amdgpu_device);
    // Clean the process cache
    struct amdsmi_process_info_cache *cache_entry, *cache_tmp;
    HASH_ITER(hh, gpu_info->last_update_process_cache, cache_entry, cache_tmp) {
      HASH_DEL(gpu_info->last_update_process_cache, cache_entry);
      free(cache_entry);
    }
  }
  free(gpu_infos);
  gpu_infos = NULL;
  amdsmi_count = 0;

  if (libamdsmi_handle) {
    dlclose(libamdsmi_handle);
    libamdsmi_handle = NULL;
    local_error_string = didnt_call_gpuinfo_init;
  }

  if (libdrm_amdgpu_handle) {
    dlclose(libdrm_amdgpu_handle);
    libdrm_amdgpu_handle = NULL;
  }
#endif
}

static const char *gpuinfo_amdsmi_last_error_string(void) {
  if (local_error_string) {
    return local_error_string;
  } else if (last_libdrm_return_status < 0) {
    switch (last_libdrm_return_status) {
    case DRM_ERR_NO_DEVICE:
      return "no device\n";
    case DRM_ERR_NO_ACCESS:
      return "no access\n";
    case DRM_ERR_NOT_ROOT:
      return "not root\n";
    case DRM_ERR_INVALID:
      return "invalid args\n";
    case DRM_ERR_NO_FD:
      return "no fd\n";
    default:
      return "unknown error\n";
    }
  } else {
    return "An unanticipated error occurred while accessing AMDGPU "
           "information\n";
  }
}

static void authenticate_drm(int fd) {
  drm_magic_t magic;

  if (_drmGetMagic(fd, &magic) < 0) {
    return;
  }

  if (_drmAuthMagic(fd, magic) == 0) {
    if (_drmDropMaster(fd)) {
      perror("Failed to drop DRM master");
      fprintf(
          stderr,
          "\nWARNING: other DRM clients will crash on VT switch while nvtop is running!\npress ENTER to continue\n");
      fgetc(stdin);
    }
    return;
  }

  // XXX: Ideally I'd implement this too, but I'd need to pull in libxcb and yet
  // more functions and structs that may break ABI compatibility.
  // See radeontop auth_xcb.c for what is involved here
  fprintf(stderr, "Failed to authenticate to DRM; XCB authentication unimplemented\n");
}

static void initDeviceSysfsPaths(struct gpu_info_amdsmi *gpu_info) {
  // Open the device sys folder to gather information not available through the DRM driver
  char devicePath[22 + PDEV_LEN];
  snprintf(devicePath, sizeof(devicePath), "/sys/bus/pci/devices/%s", gpu_info->base.pdev);
  nvtop_device_new_from_syspath(&gpu_info->amdgpuDevice, devicePath);
  assert(gpu_info->amdgpuDevice != NULL);

  gpu_info->hwmonDevice = nvtop_device_get_hwmon(gpu_info->amdgpuDevice);
  if (gpu_info->hwmonDevice) {
    // Open the device hwmon folder (Fan speed are available there)
    const char *hwmonPath;
    nvtop_device_get_syspath(gpu_info->hwmonDevice, &hwmonPath);
    int hwmonFD = open(hwmonPath, O_RDONLY);

    // Look for which fan to use (PWM or RPM)
    gpu_info->fanSpeedFILE = NULL;
    unsigned pwmIsEnabled;
    int NreadPatterns = readAttributeFromDevice(gpu_info->hwmonDevice, "pwm1_enable", "%u", &pwmIsEnabled);
    bool usePWMSensor = NreadPatterns == 1 && pwmIsEnabled > 0;

    bool useRPMSensor = false;
    if (!usePWMSensor) {
      unsigned rpmIsEnabled;
      NreadPatterns = readAttributeFromDevice(gpu_info->hwmonDevice, "fan1_enable", "%u", &rpmIsEnabled);
      useRPMSensor = NreadPatterns && rpmIsEnabled > 0;
    }
    // Either RPM or PWM or neither
    assert((useRPMSensor ^ usePWMSensor) || (!useRPMSensor && !usePWMSensor));
    if (usePWMSensor || useRPMSensor) {
      char *maxFanSpeedFile = usePWMSensor ? "pwm1_max" : "fan1_max";
      char *fanSensorFile = usePWMSensor ? "pwm1" : "fan1_input";
      unsigned maxSpeedVal;
      NreadPatterns = readAttributeFromDevice(gpu_info->hwmonDevice, maxFanSpeedFile, "%u", &maxSpeedVal);
      if (NreadPatterns == 1) {
        gpu_info->maxFanValue = maxSpeedVal;
        // Open the fan file for dynamic info gathering
        int fanSpeedFD = openat(hwmonFD, fanSensorFile, O_RDONLY);
        if (fanSpeedFD >= 0) {
          gpu_info->fanSpeedFILE = fdopen(fanSpeedFD, "r");
          if (!gpu_info->fanSpeedFILE)
            close(fanSpeedFD);
        }
      }
    }
    // Open the power cap file for dynamic info gathering
    gpu_info->powerCap = NULL;
    int powerCapFD = openat(hwmonFD, "power1_cap", O_RDONLY);
    if (powerCapFD) {
      gpu_info->powerCap = fdopen(powerCapFD, "r");
    }
    close(hwmonFD);
  }

  int sysfsFD = open(devicePath, O_RDONLY);
  // Open the PCIe bandwidth file for dynamic info gathering
  gpu_info->PCIeBW = NULL;
  int pcieBWFD = openat(sysfsFD, "pcie_bw", O_RDONLY);
  if (pcieBWFD) {
    gpu_info->PCIeBW = fdopen(pcieBWFD, "r");
  }

  close(sysfsFD);
}

#define VENDOR_AMD 0x1002

// Structure to hold temporary handle info for sorting
struct temp_handle_info {
  amdsmi_processor_handle handle;
  uuid_t uuid;
  char uuid_str[37];
  uint64_t bdf;
};

// Comparison function for sorting temp_handle_info by UUID then BDF
static int compare_handle_info(const void *a, const void *b) {
  const struct temp_handle_info *info_a = (const struct temp_handle_info *)a;
  const struct temp_handle_info *info_b = (const struct temp_handle_info *)b;

  int uuid_cmp = uuid_compare(info_a->uuid, info_b->uuid);
  if (uuid_cmp != 0) {
    return uuid_cmp;
  }
  // Secondary sort by BDF to ensure consistent ordering within a physical GPU
  if (info_a->bdf < info_b->bdf)
    return -1;
  if (info_a->bdf > info_b->bdf)
    return 1;
  return 0;
}

static bool gpuinfo_amdsmi_get_device_handles(struct list_head *devices, unsigned *count) {
#ifdef HAVE_AMDSMI
  if (!libamdsmi_handle || !_amdsmi_get_processor_handles || !_amdsmi_get_processor_type ||
      !_amdsmi_get_gpu_device_uuid || !_amdsmi_get_gpu_device_bdf || !_amdsmi_status_code_to_string) {
    local_error_string = "AMD SMI library or functions not loaded";
    return false;
  }

  amdsmi_status_t ret;
  uint32_t handle_count = 0;

  // Get the number of handles
  ret = _amdsmi_get_processor_handles(NULL, &handle_count);
  if (ret != AMDSMI_STATUS_SUCCESS || handle_count == 0) {
    _amdsmi_status_code_to_string(ret, &local_error_string);
    return false;
  }

  // Allocate memory for handles
  amdsmi_processor_handle *handles = malloc(handle_count * sizeof(amdsmi_processor_handle));
  if (!handles) {
    local_error_string = strerror(errno);
    return false;
  }

  // Get the actual handles
  ret = _amdsmi_get_processor_handles(handles, &handle_count);
  if (ret != AMDSMI_STATUS_SUCCESS) {
    _amdsmi_status_code_to_string(ret, &local_error_string);
    free(handles);
    return false;
  }

  // Filter handles, get UUIDs and BDFs for sorting
  struct temp_handle_info *temp_handles = calloc(handle_count, sizeof(struct temp_handle_info));
  if (!temp_handles) {
    local_error_string = strerror(errno);
    free(handles);
    return false;
  }

  uint32_t gpu_handle_count = 0;
  for (uint32_t i = 0; i < handle_count; ++i) {
    amdsmi_processor_type_t proc_type;
    ret = _amdsmi_get_processor_type(handles[i], &proc_type);
    if (ret != AMDSMI_STATUS_SUCCESS || proc_type != AMDSMI_PROCESSOR_TYPE_GPU) {
      continue; // Skip non-GPU handles
    }

    temp_handles[gpu_handle_count].handle = handles[i];

    // Get UUID
    uuid_t current_uuid;
    ret = _amdsmi_get_gpu_device_uuid(handles[i], current_uuid);
    if (ret != AMDSMI_STATUS_SUCCESS) {
      _amdsmi_status_code_to_string(ret, &local_error_string);
      // Continue processing other devices, but log this one failed?
      continue;
    }
    uuid_copy(temp_handles[gpu_handle_count].uuid, current_uuid);
    uuid_unparse_lower(current_uuid, temp_handles[gpu_handle_count].uuid_str);

    // Get BDF for sorting
    ret = _amdsmi_get_gpu_device_bdf(handles[i], &temp_handles[gpu_handle_count].bdf);
    if (ret != AMDSMI_STATUS_SUCCESS) {
      _amdsmi_status_code_to_string(ret, &local_error_string);
      // Continue processing other devices, but log this one failed?
      continue;
    }

    gpu_handle_count++;
  }
  free(handles); // Free original handle list

  if (gpu_handle_count == 0) {
    local_error_string = "No AMD GPUs found via SMI";
    free(temp_handles);
    return false;
  }

  // Sort handles by physical UUID, then BDF
  qsort(temp_handles, gpu_handle_count, sizeof(struct temp_handle_info), compare_handle_info);

  // Allocate gpu_infos based on the actual GPU count
  gpu_infos = calloc(gpu_handle_count, sizeof(*gpu_infos));
  if (!gpu_infos) {
    local_error_string = strerror(errno);
    free(temp_handles);
    return false;
  }

  // Populate gpu_infos from sorted handles, assigning partition index
  amdsmi_count = 0;
  unsigned current_partition_index = 0;
  uuid_t last_uuid;
  uuid_clear(last_uuid);

  for (uint32_t i = 0; i < gpu_handle_count; ++i) {
    // Check if UUID changed to reset partition index
    if (uuid_compare(last_uuid, temp_handles[i].uuid) != 0) {
      current_partition_index = 0;
      uuid_copy(last_uuid, temp_handles[i].uuid);
    }

    gpu_infos[amdsmi_count].processor_handle = temp_handles[i].handle;
    strncpy(gpu_infos[amdsmi_count].physical_gpu_uuid_str, temp_handles[i].uuid_str, 37);
    gpu_infos[amdsmi_count].partition_index = current_partition_index;
    gpu_infos[amdsmi_count].base.vendor = &gpu_vendor_amdsmi;

    // Populate pdev with BDF string format (Domain:Bus:Device.Function)
    uint64_t bdf = temp_handles[i].bdf;
    uint32_t domain = (bdf >> 32) & 0xFFFFFFFF;
    uint8_t bus = (bdf >> 8) & 0xFF;
    uint8_t device = (bdf >> 3) & 0x1F;
    uint8_t func = bdf & 0x7;
    snprintf(gpu_infos[amdsmi_count].base.pdev, PDEV_LEN - 1, "%04x:%02x:%02x.%d", domain, bus, device, func);

    // Add to the list
    list_add_tail(&gpu_infos[amdsmi_count].base.list, devices);
    amdsmi_count++;
    current_partition_index++;
  }

  free(temp_handles);
  *count = amdsmi_count;
  local_error_string = NULL;
  return true;

#else // Fallback to libdrm
  if (!libdrm_handle)
    return false;

  last_libdrm_return_status = wrap_drmGetDevices(NULL, 0);
  if (last_libdrm_return_status <= 0)
    return false;

  drmDevicePtr devs[last_libdrm_return_status];
  last_libdrm_return_status = wrap_drmGetDevices(devs, last_libdrm_return_status);
  if (last_libdrm_return_status <= 0)
    return false;

  unsigned int libdrm_count = last_libdrm_return_status;
  gpu_infos = calloc(libdrm_count, sizeof(*gpu_infos));
  if (!gpu_infos) {
    local_error_string = strerror(errno);
    return false;
  }

  for (unsigned int i = 0; i < libdrm_count; i++) {
    if (devs[i]->bustype != DRM_BUS_PCI || devs[i]->deviceinfo.pci->vendor_id != VENDOR_AMD)
      continue;

    int fd = -1;

    // Try render node first
    if (1 << DRM_NODE_RENDER & devs[i]->available_nodes) {
      fd = open(devs[i]->nodes[DRM_NODE_RENDER], O_RDWR);
    }
    if (fd < 0) {
      // Fallback to primary node (control nodes are unused according to the DRM documentation)
      if (1 << DRM_NODE_PRIMARY & devs[i]->available_nodes) {
        fd = open(devs[i]->nodes[DRM_NODE_PRIMARY], O_RDWR);
      }
    }

    if (fd < 0)
      continue;

    drmVersionPtr ver = _drmGetVersion(fd);

    if (!ver) {
      close(fd);
      continue;
    }

    bool is_radeon = false; // TODO: !strcmp(ver->name, "radeon");
    bool is_amdgpu = !strcmp(ver->name, "amdgpu");

    if (!is_amdgpu && !is_radeon) {
      _drmFreeVersion(ver);
      close(fd);
      continue;
    }

    authenticate_drm(fd);

    if (is_amdgpu) {
      if (!libdrm_amdgpu_handle || !_amdgpu_device_initialize) {
        _drmFreeVersion(ver);
        close(fd);
        continue;
      }

      uint32_t drm_major, drm_minor;
      last_libdrm_return_status =
          _amdgpu_device_initialize(fd, &drm_major, &drm_minor, &gpu_infos[amdsmi_count].amdgpu_device);
    } else {
      // TODO: radeon suppport here
      assert(false);
    }

    if (!last_libdrm_return_status) {
      gpu_infos[amdsmi_count].drmVersion = ver;
      gpu_infos[amdsmi_count].fd = fd;
      gpu_infos[amdsmi_count].base.vendor = &gpu_vendor_amdsmi;

      snprintf(gpu_infos[amdsmi_count].base.pdev, PDEV_LEN - 1, "%04x:%02x:%02x.%d", devs[i]->businfo.pci->domain,
               devs[i]->businfo.pci->bus, devs[i]->businfo.pci->dev, devs[i]->businfo.pci->func);
      initDeviceSysfsPaths(&gpu_infos[amdsmi_count]);
      list_add_tail(&gpu_infos[amdsmi_count].base.list, devices);
      // Register a fdinfo callback for this GPU
      processinfo_register_fdinfo_callback(parse_drm_fdinfo_amd, &gpu_infos[amdsmi_count].base);
      amdsmi_count++;
    } else {
      _drmFreeVersion(ver);
      close(fd);
      continue;
    }
  }

  _drmFreeDevices(devs, libdrm_count);
  *count = amdsmi_count;

  return true;
#endif
}

static int rewindAndReadPattern(FILE *file, const char *format, ...) {
  if (!file)
    return 0;
  va_list args;
  va_start(args, format);
  rewind(file);
  fflush(file);
  int matches = vfscanf(file, format, args);
  va_end(args);
  return matches;
}

static int readAttributeFromDevice(nvtop_device *dev, const char *sysAttr, const char *format, ...) {
  va_list args;
  va_start(args, format);
  const char *val;
  int ret = nvtop_device_get_sysattr_value(dev, sysAttr, &val);
  if (ret < 0) {
    va_end(args);
    return ret;
  }
  // Read the pattern
  int nread = vsscanf(val, format, args);
  va_end(args);
  return nread;
}

// Helper to convert GT/s to PCIe Gen (approximate)
static unsigned pcie_gen_from_speed_gts(double speed_gts) {
    if (speed_gts >= 31.5) return 5;
    if (speed_gts >= 15.5) return 4;
    if (speed_gts >= 7.5) return 3;
    if (speed_gts >= 4.5) return 2;
    if (speed_gts >= 2.0) return 1;
    return 0;
}

static void gpuinfo_amdsmi_populate_static_info(struct gpu_info *_gpu_info) {
#ifdef HAVE_AMDSMI
  struct gpu_info_amdsmi *gpu_info = container_of(_gpu_info, struct gpu_info_amdsmi, base);
  struct gpuinfo_static_info *static_info = &gpu_info->base.static_info;
  amdsmi_status_t ret;

  RESET_ALL(static_info->valid);
  static_info->integrated_graphics = false; // Default, might be updated
  static_info->encode_decode_shared = false; // Default, might be updated

  // 1. Device Name
  amdsmi_asic_info_t asic_info;
  ret = _amdsmi_get_gpu_asic_info(gpu_info->processor_handle, &asic_info);
  if (ret == AMDSMI_STATUS_SUCCESS && strlen(asic_info.market_name) > 0) {
    strncpy(static_info->device_name, asic_info.market_name, MAX_DEVICE_NAME - 1);
  } else {
    // Fallback to board name if market name fails
    amdsmi_board_info_t board_info;
    ret = _amdsmi_get_gpu_board_info(gpu_info->processor_handle, &board_info);
    if (ret == AMDSMI_STATUS_SUCCESS && strlen(board_info.product_name) > 0) {
      strncpy(static_info->device_name, board_info.product_name, MAX_DEVICE_NAME - 1);
    } else {
      // Last resort: use BDF
      strncpy(static_info->device_name, gpu_info->base.pdev, MAX_DEVICE_NAME - 1);
    }
  }
  static_info->device_name[MAX_DEVICE_NAME - 1] = '\0';

  // Add partition info if applicable (multiple handles share the same UUID)
  // We need the total count of GPUs/partitions found earlier to determine this.
  // Let's assume `amdsmi_count` holds the total count found in get_device_handles.
  // This check needs refinement - we need to know if *this specific UUID* has multiple handles.
  // TODO: Improve this check later, maybe pass total count for this UUID.
  if (amdsmi_count > 1) { // Simplistic check for now
      char partition_suffix[32];
      snprintf(partition_suffix, sizeof(partition_suffix), " [Part %u]", gpu_info->partition_index);
      strncat(static_info->device_name, partition_suffix, MAX_DEVICE_NAME - 1 - strlen(static_info->device_name));
  }
  SET_VALID(gpuinfo_device_name_valid, static_info->valid);

  // 2. Temperature Thresholds (Optional, requires specific functions not loaded yet)
  // Placeholder: If needed, load and call amdsmi_get_temp_metric for CRITICAL/EMERGENCY
  // int64_t temp_crit_mc, temp_emerg_mc;
  // if (_amdsmi_get_temp_metric && _amdsmi_get_temp_metric(gpu_info->processor_handle, AMDSMI_TEMP_TYPE_EDGE, AMDSMI_TEMP_METRIC_CRITICAL, &temp_crit_mc) == AMDSMI_STATUS_SUCCESS) {
  //    SET_GPUINFO_STATIC(static_info, temperature_slowdown_threshold, temp_crit_mc / 1000);
  // }
  // if (_amdsmi_get_temp_metric && _amdsmi_get_temp_metric(gpu_info->processor_handle, AMDSMI_TEMP_TYPE_EDGE, AMDSMI_TEMP_METRIC_EMERGENCY, &temp_emerg_mc) == AMDSMI_STATUS_SUCCESS) {
  //    SET_GPUINFO_STATIC(static_info, temperature_shutdown_threshold, temp_emerg_mc / 1000);
  // }

  // 3. Max PCIe Link Info
  if (_amdsmi_get_pcie_link_caps) {
      amdsmi_pcie_link_caps_t pcie_caps;
      ret = _amdsmi_get_pcie_link_caps(gpu_info->processor_handle, &pcie_caps);
      if (ret == AMDSMI_STATUS_SUCCESS) {
          SET_GPUINFO_STATIC(static_info, max_pcie_link_width, pcie_caps.max_lanes);
          // Speed is often reported directly in GT/s in the API struct, needs verification
          // Assuming pcie_caps.max_speed is in GT/s (needs confirmation from amdsmi.h)
          // double max_speed_gts = pcie_caps.max_speed; // Adjust based on actual units
          // SET_GPUINFO_STATIC(static_info, max_pcie_gen, pcie_gen_from_speed_gts(max_speed_gts));
          // Alternatively, pcie_caps might directly contain generation info.
      }
  }

  // 4. Integrated Graphics (Heuristic based on name? Requires more reliable method)
  // TODO: Find a better way using AMDSMI, perhaps vendor/device ID lookup?
  if (strstr(static_info->device_name, "Radeon Graphics") || strstr(static_info->device_name, "Ryzen")) {
      static_info->integrated_graphics = true;
  }

  // 5. Encode/Decode Shared (Heuristic/Default for now)
  // TODO: Find a better way using AMDSMI, maybe check IP block info if available?
  static_info->encode_decode_shared = false; // Default assumption

  // 6. Total VRAM for this partition/GPU
  if (_amdsmi_get_gpu_vram_info) {
      amdsmi_vram_info_t vram_info;
      ret = _amdsmi_get_gpu_vram_info(gpu_info->processor_handle, &vram_info);
      if (ret == AMDSMI_STATUS_SUCCESS) {
          SET_GPUINFO_STATIC(static_info, total_memory, vram_info.vram_size_bytes); // Assuming this is per-handle
          // Note: nvtop internally uses total_memory in dynamic_info, let's set it there too
          // or adjust nvtop's usage later.
      } else {
          // Maybe fallback to usage total if info fails?
          amdsmi_vram_usage_t vram_usage;
          ret = _amdsmi_get_gpu_vram_usage(gpu_info->processor_handle, &vram_usage);
          if (ret == AMDSMI_STATUS_SUCCESS) {
              SET_GPUINFO_STATIC(static_info, total_memory, vram_usage.vram_total);
          }
      }
  }

  // 7. Max Clocks (GFX/MEM)
  if (_amdsmi_get_clock_info) {
      amdsmi_clk_info_t clk_info;
      ret = _amdsmi_get_clock_info(gpu_info->processor_handle, AMDSMI_CLK_TYPE_GFX, &clk_info);
      if (ret == AMDSMI_STATUS_SUCCESS) {
          SET_GPUINFO_STATIC(static_info, gpu_clock_speed_max, clk_info.max_clk);
      }
      ret = _amdsmi_get_clock_info(gpu_info->processor_handle, AMDSMI_CLK_TYPE_MEM, &clk_info);
      if (ret == AMDSMI_STATUS_SUCCESS) {
          SET_GPUINFO_STATIC(static_info, mem_clock_speed_max, clk_info.max_clk);
      }
  }

#else // Fallback to libdrm/sysfs
  struct gpu_info_amdsmi *gpu_info = container_of(_gpu_info, struct gpu_info_amdsmi, base);
  struct gpuinfo_static_info *static_info = &gpu_info->base.static_info;
  bool info_query_success = false;
  struct amdgpu_gpu_info info;
  const char *name = NULL;

  static_info->integrated_graphics = false;
  static_info->encode_decode_shared = false;
  RESET_ALL(static_info->valid);

  if (libdrm_amdgpu_handle && _amdgpu_get_marketing_name)
    name = _amdgpu_get_marketing_name(gpu_info->amdgpu_device);

  if (libdrm_amdgpu_handle && _amdgpu_query_gpu_info)
    info_query_success = !_amdgpu_query_gpu_info(gpu_info->amdgpu_device, &info);

  /* check name again.
   * the previous name is from libdrm, which may not be the latest version.
   * it may not contain latest AMD GPU types/names
   *
   * the libdrm is from vendor, Linux and a Linux distribution.
   * It may take long time for a Linux distribution to get latest GPU info.
   * here a GPU IDS is maintained, which allows to support GPU info faster. */
  if (!name) {
    name = amdgpu_parse_marketing_name(&info);
  }

  static_info->device_name[MAX_DEVICE_NAME - 1] = '\0';
  if (name && strlen(name)) {
    strncpy(static_info->device_name, name, MAX_DEVICE_NAME - 1);
    SET_VALID(gpuinfo_device_name_valid, static_info->valid);
  } else if (gpu_info->drmVersion->desc && strlen(gpu_info->drmVersion->desc)) {
    strncpy(static_info->device_name, gpu_info->drmVersion->desc, MAX_DEVICE_NAME - 1);
    SET_VALID(gpuinfo_device_name_valid, static_info->valid);

    if (info_query_success) {
      size_t len = strlen(static_info->device_name);
      assert(len < MAX_DEVICE_NAME);

      char *dst = static_info->device_name + len;
      size_t remaining_len = MAX_DEVICE_NAME - 1 - len;
      switch (info.family_id) {
#ifdef AMDGPU_FAMILY_SI
      case AMDGPU_FAMILY_SI:
        strncpy(dst, " (Hainan / Oland / Verde / Pitcairn / Tahiti)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_CI
      case AMDGPU_FAMILY_CI:
        strncpy(dst, " (Bonaire / Hawaii)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_KV
      case AMDGPU_FAMILY_KV:
        strncpy(dst, " (Kaveri / Kabini / Mullins)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_VI
      case AMDGPU_FAMILY_VI:
        strncpy(dst, " (Iceland / Tonga)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_CZ
      case AMDGPU_FAMILY_CZ:
        strncpy(dst, " (Carrizo / Stoney)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_AI
      case AMDGPU_FAMILY_AI:
        strncpy(dst, " (Vega10)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_RV
      case AMDGPU_FAMILY_RV:
        strncpy(dst, " (Raven)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_NV
      case AMDGPU_FAMILY_NV:
        strncpy(dst, " (Navi10)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_VGH
      case AMDGPU_FAMILY_VGH:
        strncpy(dst, " (Van Gogh)", remaining_len);
        break;
#endif
#ifdef AMDGPU_FAMILY_YC
      case AMDGPU_FAMILY_YC:
        strncpy(dst, " (Yellow Carp)", remaining_len);
        break;
#endif
      default:
        break;
      }
    }
  }

  // Retrieve infos from sysfs.

  // 1) Fan
  // If multiple fans are present, use the first one. Some hardware do not wire
  // the sensor for the second fan, or use the same value as the first fan.

  // Critical temparature
  // temp1_* files should always be the GPU die in millidegrees Celsius
  if (gpu_info->hwmonDevice) {
    unsigned criticalTemp;
    int NreadPatterns = readAttributeFromDevice(gpu_info->hwmonDevice, "temp1_crit", "%u", &criticalTemp);
    if (NreadPatterns == 1) {
      SET_GPUINFO_STATIC(static_info, temperature_slowdown_threshold, criticalTemp);
    }

    // Emergency/shutdown temparature
    unsigned emergemcyTemp;
    NreadPatterns = readAttributeFromDevice(gpu_info->hwmonDevice, "temp1_emergency", "%u", &emergemcyTemp);
    if (NreadPatterns == 1) {
      SET_GPUINFO_STATIC(static_info, temperature_shutdown_threshold, emergemcyTemp);
    }
  }

  nvtop_pcie_link max_link_characteristics;
  int ret = nvtop_device_maximum_pcie_link(gpu_info->amdgpuDevice, &max_link_characteristics);
  if (ret >= 0) {
    SET_GPUINFO_STATIC(static_info, max_pcie_link_width, max_link_characteristics.width);
    unsigned pcieGen = nvtop_pcie_gen_from_link_speed(max_link_characteristics.speed);
    SET_GPUINFO_STATIC(static_info, max_pcie_gen, pcieGen);
  }

  // Mark integrated graphics
  if (info_query_success && (info.ids_flags & AMDGPU_IDS_FLAGS_FUSION)) {
    static_info->integrated_graphics = true;
  }

  // Checking if Encode and Decode are unified:AMDGPU_INFO_HW_IP_INFO
  if (_amdgpu_query_hw_ip_info) {
    struct drm_amdgpu_info_hw_ip vcn_ip_info;
    if (_amdgpu_query_hw_ip_info(gpu_info->amdgpu_device, AMDGPU_HW_IP_VCN_ENC, 0, &vcn_ip_info) == 0) {
      static_info->encode_decode_shared = vcn_ip_info.hw_ip_version_major >= 4;
    }
  }
#endif
}

static void gpuinfo_amdsmi_refresh_dynamic_info(struct gpu_info *_gpu_info) {
  struct gpu_info_amdsmi *gpu_info = container_of(_gpu_info, struct gpu_info_amdsmi, base);
  struct gpuinfo_dynamic_info *dynamic_info = &gpu_info->base.dynamic_info;
  RESET_ALL(dynamic_info->valid);

#ifdef HAVE_AMDSMI
  if (!_amdsmi_get_gpu_metrics) {
    // Fallback or error handling if function pointer is null
    local_error_string = "amdsmi_get_gpu_metrics not loaded";
    // Consider falling back to libdrm/sysfs if appropriate, or just return
    goto amdsmi_fallback; // Use goto to jump to the fallback code
  }

  amdsmi_gpu_metrics_t metrics;
  amdsmi_status_t ret = _amdsmi_get_gpu_metrics(gpu_info->processor_handle, &metrics);

  if (ret == AMDSMI_STATUS_SUCCESS) {
    // --- GPU Clocks ---
    if (metrics.socket_power_valid) // Use socket_power_valid as a general indicator? Check metrics doc.
    {
      if (metrics.average_gfxclk_frequency_valid)
        SET_GPUINFO_DYNAMIC(dynamic_info, gpu_clock_speed, metrics.average_gfxclk_frequency);
      if (metrics.gfxclk_max_freq_valid) // Assuming a max clock field exists, adjust if name differs
         SET_GPUINFO_DYNAMIC(dynamic_info, gpu_clock_speed_max, metrics.gfxclk_max_freq); // Adjust field name if needed
    }

    // --- Memory Clocks ---
    if (metrics.average_uclk_frequency_valid)
      SET_GPUINFO_DYNAMIC(dynamic_info, mem_clock_speed, metrics.average_uclk_frequency);
     if (metrics.uclk_max_freq_valid) // Assuming a max clock field exists, adjust if name differs
        SET_GPUINFO_DYNAMIC(dynamic_info, mem_clock_speed_max, metrics.uclk_max_freq); // Adjust field name if needed


    // --- Utilization ---
    if (metrics.average_gfx_activity_valid)
      SET_GPUINFO_DYNAMIC(dynamic_info, gpu_util_rate, metrics.average_gfx_activity);

    // --- Memory Usage ---
    // Note: amdsmi_gpu_metrics_t might not directly provide total/used/free memory.
    // We might need to keep the amdsmi_get_gpu_vram_usage call or use info from static populate.
    // Let's check if metrics provides vram usage percentage directly
     if (metrics.vram_usage_valid) {
       // Assuming total memory is already populated in static info
       if (gpu_info->base.static_info.total_memory > 0) {
         uint64_t used_mem = (uint64_t)metrics.vram_usage * gpu_info->base.static_info.total_memory / 100;
         SET_GPUINFO_DYNAMIC(dynamic_info, used_memory, used_mem);
         SET_GPUINFO_DYNAMIC(dynamic_info, free_memory, gpu_info->base.static_info.total_memory - used_mem);
         SET_GPUINFO_DYNAMIC(dynamic_info, mem_util_rate, metrics.vram_usage);
       }
     } else {
        // Fallback: Query VRAM usage separately if metrics doesn't provide it
        amdsmi_vram_usage_t vram_info;
        if (_amdsmi_get_gpu_vram_usage && _amdsmi_get_gpu_vram_usage(gpu_info->processor_handle, &vram_info) == AMDSMI_STATUS_SUCCESS) {
           SET_GPUINFO_DYNAMIC(dynamic_info, used_memory, vram_info.vram_used);
            // Assuming total memory is already populated in static info
            if (gpu_info->base.static_info.total_memory > 0) {
                 SET_GPUINFO_DYNAMIC(dynamic_info, free_memory, gpu_info->base.static_info.total_memory - vram_info.vram_used);
                 SET_GPUINFO_DYNAMIC(dynamic_info, mem_util_rate, (uint32_t)(vram_info.vram_used * 100 / gpu_info->base.static_info.total_memory));
            }
        }
     }


    // --- Temperature ---
    // amdsmi_gpu_metrics_t may contain multiple temperature sensors (junction, edge, mem)
    // Choosing 'edge' temperature as the default GPU temp for now.
    if (metrics.temperature_edge_valid)
      SET_GPUINFO_DYNAMIC(dynamic_info, gpu_temp, metrics.temperature_edge); // Temperature likely in Celsius

    // --- Fan Speed ---
    // Check if metrics contains fan speed percentage
    if (metrics.fan_speed_rpm_valid) // Assuming an RPM field, might need conversion to %
    {
        // If metrics gives RPM, we might need max RPM from static info or another call
        // Let's assume metrics.current_fan_speed is percentage for now, adjust if needed.
        if (metrics.current_fan_speed_valid)
             SET_GPUINFO_DYNAMIC(dynamic_info, fan_speed, metrics.current_fan_speed); // Assuming this is percentage
         else {
             // Fallback: Query fan speed separately
             uint32_t fan_speed_val = 0;
             uint64_t fan_speed_rpm = 0; // Use uint64_t for RPM
             if (_amdsmi_get_gpu_fan_speed && _amdsmi_get_gpu_fan_speed(gpu_info->processor_handle, 0, &fan_speed_rpm) == AMDSMI_STATUS_SUCCESS) {
                 // Need max RPM to calculate percentage. Placeholder: Assume 100% if RPM > 0 for now.
                 // TODO: Get max fan speed (e.g., from static info or another amdsmi call)
                 // For now, just indicate activity if RPM > 0
                 // If we have max fan speed (e.g., gpu_info->maxFanRpm):
                 // fan_speed_val = (uint32_t)(fan_speed_rpm * 100 / gpu_info->maxFanRpm);
                 // Placeholder logic:
                 fan_speed_val = (fan_speed_rpm > 0) ? 100 : 0; // Very basic placeholder
                 SET_GPUINFO_DYNAMIC(dynamic_info, fan_speed, fan_speed_val);
             }
         }
    }


    // --- Power Draw ---
    if (metrics.average_socket_power_valid)
      SET_GPUINFO_DYNAMIC(dynamic_info, power_draw, (uint64_t)metrics.average_socket_power * 1000); // Assuming power is in Watts, convert to mW

    // --- PCIe --- - Keep existing logic for now, potentially refine later
    nvtop_pcie_link curr_link_characteristics;
    int ret_pcie = nvtop_device_current_pcie_link(gpu_info->amdgpuDevice, &curr_link_characteristics);
    if (ret_pcie >= 0) {
      SET_GPUINFO_DYNAMIC(dynamic_info, pcie_link_width, curr_link_characteristics.width);
      unsigned pcieGen = nvtop_pcie_gen_from_link_speed(curr_link_characteristics.speed);
      SET_GPUINFO_DYNAMIC(dynamic_info, pcie_link_gen, pcieGen);
    }

    // Try getting PCIe bandwidth from AMDSMI if available
    amdsmi_pcie_bandwidth_t pcie_bw;
     if (_amdsmi_get_pcie_bandwidth && _amdsmi_get_pcie_bandwidth(gpu_info->processor_handle, &pcie_bw) == AMDSMI_STATUS_SUCCESS && pcie_bw.pcie_bandwidth_inst_valid) {
        // Convert bytes per second to KiB/s
        SET_GPUINFO_DYNAMIC(dynamic_info, pcie_rx, pcie_bw.pcie_bandwidth_inst[AMDSMI_PCIE_BW_RECEIVED] / 1024);
        SET_GPUINFO_DYNAMIC(dynamic_info, pcie_tx, pcie_bw.pcie_bandwidth_inst[AMDSMI_PCIE_BW_SENT] / 1024);
     } else if (gpu_info->PCIeBW) { // Fallback to sysfs
      uint64_t received, transmitted;
      int maxPayloadSize;
      int NreadPatterns =
          rewindAndReadPattern(gpu_info->PCIeBW, "%\" SCNu64 \" %\" SCNu64 \" %i\", &received, &transmitted, &maxPayloadSize);
      if (NreadPatterns == 3) {
        received *= maxPayloadSize;
        transmitted *= maxPayloadSize;
        received /= 1024;
        transmitted /= 1024;
        SET_GPUINFO_DYNAMIC(dynamic_info, pcie_rx, received);
        SET_GPUINFO_DYNAMIC(dynamic_info, pcie_tx, transmitted);
      }
    }


    // --- Power Cap --- - Keep existing logic for now, potentially refine later
    // Can potentially use _amdsmi_get_power_cap_info here
    if (gpu_info->powerCap) {
      unsigned powerCap;
      int NreadPatterns = rewindAndReadPattern(gpu_info->powerCap, \"%u\", &powerCap);
      if (NreadPatterns == 1) {
        SET_GPUINFO_DYNAMIC(dynamic_info, power_draw_max, powerCap / 1000);
      }
    }

    return; // Successfully populated using AMDSMI metrics

  } else {
    // Log error or handle failure to get metrics
    char error_buf[256];
    snprintf(error_buf, sizeof(error_buf), "amdsmi_get_gpu_metrics failed: %s",
             _amdsmi_status_code_to_string ? _amdsmi_status_code_to_string(ret) : "Unknown AMDSMI error");
    local_error_string = strdup(error_buf); // Note: needs free later if strdup used
    // Fall through to libdrm/sysfs fallback path
  }

amdsmi_fallback:; // Label for goto jump

#else // Fallback to libdrm/sysfs if HAVE_AMDSMI is not defined

  bool info_query_success = false;
  struct amdgpu_gpu_info info;
  uint32_t out32;

  // RESET_ALL(dynamic_info->valid); // Already done at the start

  if (libdrm_amdgpu_handle && _amdgpu_query_gpu_info)
    info_query_success = !_amdgpu_query_gpu_info(gpu_info->amdgpu_device, &info);

  // GPU current speed
  if (libdrm_amdgpu_handle && _amdgpu_query_sensor_info)
    last_libdrm_return_status =
        _amdgpu_query_sensor_info(gpu_info->amdgpu_device, AMDGPU_INFO_SENSOR_GFX_SCLK, sizeof(out32), &out32);
  else
    last_libdrm_return_status = 1;
  if (!last_libdrm_return_status) {
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_clock_speed, out32);
  }

  // GPU max speed
  if (info_query_success) {
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_clock_speed_max, info.max_engine_clk / 1000);
  }

  // Memory current speed
  if (libdrm_amdgpu_handle && _amdgpu_query_sensor_info)
    last_libdrm_return_status =
        _amdgpu_query_sensor_info(gpu_info->amdgpu_device, AMDGPU_INFO_SENSOR_GFX_MCLK, sizeof(out32), &out32);
  else
    last_libdrm_return_status = 1;
  if (!last_libdrm_return_status) {
    SET_GPUINFO_DYNAMIC(dynamic_info, mem_clock_speed, out32);
  }

  // Memory max speed
  if (info_query_success) {
    SET_GPUINFO_DYNAMIC(dynamic_info, mem_clock_speed_max, info.max_memory_clk / 1000);
  }

  // Load
  if (libdrm_amdgpu_handle && _amdgpu_query_sensor_info)
    last_libdrm_return_status =
        _amdgpu_query_sensor_info(gpu_info->amdgpu_device, AMDGPU_INFO_SENSOR_GPU_LOAD, sizeof(out32), &out32);
  else
    last_libdrm_return_status = 1;
  if (!last_libdrm_return_status) {
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_util_rate, out32);
  }

  // Memory usage
  struct drm_amdgpu_memory_info memory_info;
  if (libdrm_amdgpu_handle && _amdgpu_query_info)
    last_libdrm_return_status =
        _amdgpu_query_info(gpu_info->amdgpu_device, AMDGPU_INFO_MEMORY, sizeof(memory_info), &memory_info);
  else
    last_libdrm_return_status = 1;
  if (!last_libdrm_return_status) {
    // TODO: Determine if we want to include GTT (GPU accessible system memory)
    // Assuming total memory populated during static phase or from vram_info above if AMDSMI
    if (gpu_info->base.static_info.total_memory == 0) {
        // Populate total memory if not already set (should be set in static)
         SET_GPUINFO_DYNAMIC(dynamic_info, total_memory, memory_info.vram.total_heap_size);
    } else {
        dynamic_info->total_memory = gpu_info->base.static_info.total_memory; // Ensure consistency
        VALIDATE_GPUINFO_DYNAMIC(dynamic_info, total_memory);
    }

    SET_GPUINFO_DYNAMIC(dynamic_info, used_memory, memory_info.vram.heap_usage);
    if (IS_VALID_GPUINFO_DYNAMIC(dynamic_info, total_memory)) {
         SET_GPUINFO_DYNAMIC(dynamic_info, free_memory, dynamic_info->total_memory - dynamic_info->used_memory);
         if (dynamic_info->total_memory > 0) {
             SET_GPUINFO_DYNAMIC(dynamic_info, mem_util_rate,
                                (dynamic_info->used_memory) * 100 / dynamic_info->total_memory);
         }
    }
  }

  // GPU temperature
  if (libdrm_amdgpu_handle && _amdgpu_query_sensor_info)
    last_libdrm_return_status =
        _amdgpu_query_sensor_info(gpu_info->amdgpu_device, AMDGPU_INFO_SENSOR_GPU_TEMP, sizeof(out32), &out32);
  else
    last_libdrm_return_status = 1;
  if (!last_libdrm_return_status) {
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_temp, out32 / 1000);
  }

  // Fan speed
  // Use sysfs if available
  if (gpu_info->fanSpeedFILE) {
    unsigned currentFanSpeed;
    int patternsMatched = rewindAndReadPattern(gpu_info->fanSpeedFILE, "%u", &currentFanSpeed);
    if (patternsMatched == 1 && gpu_info->maxFanValue > 0) { // Check maxFanValue
      SET_GPUINFO_DYNAMIC(dynamic_info, fan_speed, currentFanSpeed * 100 / gpu_info->maxFanValue);
    }
  }


  // Device power usage
  if (libdrm_amdgpu_handle && _amdgpu_query_sensor_info)
    last_libdrm_return_status =
        _amdgpu_query_sensor_info(gpu_info->amdgpu_device, AMDGPU_INFO_SENSOR_GPU_AVG_POWER, sizeof(out32), &out32);
  else
    last_libdrm_return_status = 1;
  if (!last_libdrm_return_status) {
    SET_GPUINFO_DYNAMIC(dynamic_info, power_draw, out32 * 1000);
  }

  // PCIe Link Info (kept the same as before)
  nvtop_pcie_link curr_link_characteristics;
  int ret = nvtop_device_current_pcie_link(gpu_info->amdgpuDevice, &curr_link_characteristics);
  if (ret >= 0) {
    SET_GPUINFO_DYNAMIC(dynamic_info, pcie_link_width, curr_link_characteristics.width);
    unsigned pcieGen = nvtop_pcie_gen_from_link_speed(curr_link_characteristics.speed);
    SET_GPUINFO_DYNAMIC(dynamic_info, pcie_link_gen, pcieGen);
  }

  // PCIe bandwidth (kept the same as before)
  if (gpu_info->PCIeBW) {
    uint64_t received, transmitted;
    int maxPayloadSize;
    int NreadPatterns =
        rewindAndReadPattern(gpu_info->PCIeBW, "%\" SCNu64 \" %\" SCNu64 \" %i\", &received, &transmitted, &maxPayloadSize);
    if (NreadPatterns == 3) {
      received *= maxPayloadSize;
      transmitted *= maxPayloadSize;
      received /= 1024;
      transmitted /= 1024;
      SET_GPUINFO_DYNAMIC(dynamic_info, pcie_rx, received);
      SET_GPUINFO_DYNAMIC(dynamic_info, pcie_tx, transmitted);
    }
  }

  // Power Cap (kept the same as before)
  if (gpu_info->powerCap) {
    unsigned powerCap;
    int NreadPatterns = rewindAndReadPattern(gpu_info->powerCap, "%u", &powerCap);
    if (NreadPatterns == 1) {
      SET_GPUINFO_DYNAMIC(dynamic_info, power_draw_max, powerCap / 1000);
    }
  }
#endif // HAVE_AMDSMI
}

static const char drm_amdgpu_pdev_old[] = "pdev";
static const char drm_amdgpu_vram_old[] = "vram mem";
static const char drm_amdgpu_vram[] = "drm-memory-vram";
static const char drm_amdgpu_gfx_old[] = "gfx";
static const char drm_amdgpu_gfx[] = "drm-engine-gfx";
static const char drm_amdgpu_compute_old[] = "compute";
static const char drm_amdgpu_compute[] = "drm-engine-compute";
static const char drm_amdgpu_dec_old[] = "dec";
static const char drm_amdgpu_dec[] = "drm-engine-dec";
static const char drm_amdgpu_enc_old[] = "enc";
static const char drm_amdgpu_enc[] = "drm-engine-enc";

static bool parse_drm_fdinfo_amd(struct gpu_info *info, FILE *fdinfo_file, struct gpu_process *process_info) {
  struct gpu_info_amdsmi *gpu_info = container_of(info, struct gpu_info_amdsmi, base);
  struct gpuinfo_static_info *static_info = &gpu_info->base.static_info;
  static char *line = NULL;
  static size_t line_buf_size = 0;
  ssize_t count = 0;

  bool client_id_set = false;
  unsigned cid;
  nvtop_time current_time;
  nvtop_get_current_time(&current_time);

  while ((count = getline(&line, &line_buf_size, fdinfo_file)) != -1) {
    char *key, *val;
    // Get rid of the newline if present
    if (line[count - 1] == '\n') {
      line[--count] = '\0';
    }

    if (!extract_drm_fdinfo_key_value(line, &key, &val))
      continue;

    // see drivers/gpu/drm/amd/amdgpu/amdgpu_fdinfo.c amdgpu_show_fdinfo()
    if (!strcmp(key, drm_amdgpu_pdev_old) || !strcmp(key, drm_pdev)) {
      if (strcmp(val, gpu_info->base.pdev)) {
        return false;
      }
    } else if (!strcmp(key, drm_client_id)) {
      // Client id is a unique identifier. From the DRM documentation "Unique value relating to the open DRM
      // file descriptor used to distinguish duplicated and shared file descriptors. Conceptually the value should map
      // 1:1 to the in kernel representation of struct drm_file instances."
      char *endptr;
      cid = strtoul(val, &endptr, 10);
      if (*endptr)
        continue;
      client_id_set = true;
    } else if (!strcmp(key, drm_amdgpu_vram_old) || !strcmp(key, drm_amdgpu_vram)) {
      // TODO: do we count "gtt mem" too?
      unsigned long mem_int;
      char *endptr;

      mem_int = strtoul(val, &endptr, 10);
      if (endptr == val || (strcmp(endptr, " kB") && strcmp(endptr, " KiB")))
        continue;

      SET_GPUINFO_PROCESS(process_info, gpu_memory_usage, mem_int * 1024);
    } else {
      bool is_gfx_old = !strncmp(key, drm_amdgpu_gfx_old, sizeof(drm_amdgpu_gfx_old) - 1);
      bool is_compute_old = !strncmp(key, drm_amdgpu_compute_old, sizeof(drm_amdgpu_compute_old) - 1);
      bool is_dec_old = !strncmp(key, drm_amdgpu_dec_old, sizeof(drm_amdgpu_dec_old) - 1);
      bool is_enc_old = !strncmp(key, drm_amdgpu_enc_old, sizeof(drm_amdgpu_enc_old) - 1);

      bool is_gfx_new = !strncmp(key, drm_amdgpu_gfx, sizeof(drm_amdgpu_gfx) - 1);
      bool is_dec_new = !strncmp(key, drm_amdgpu_dec, sizeof(drm_amdgpu_dec) - 1);
      bool is_enc_new = !strncmp(key, drm_amdgpu_enc, sizeof(drm_amdgpu_enc) - 1);
      bool is_compute_new = !strncmp(key, drm_amdgpu_compute, sizeof(drm_amdgpu_compute) - 1);

      if (is_gfx_old || is_compute_old || is_dec_old || is_enc_old) {
        // The old interface exposes a usage percentage with an unknown update interval
        unsigned int usage_percent_int;
        char *key_off, *endptr;
        double usage_percent;

        if (is_gfx_old)
          key_off = key + sizeof(drm_amdgpu_gfx_old) - 1;
        else if (is_compute_old)
          key_off = key + sizeof(drm_amdgpu_compute_old) - 1;
        else if (is_dec_old)
          key_off = key + sizeof(drm_amdgpu_dec_old) - 1;
        else if (is_enc_old)
          key_off = key + sizeof(drm_amdgpu_enc_old) - 1;
        else
          continue;

        // The prefix should be followed by a number and only a number
        if (!*key_off)
          continue;
        strtoul(key_off, &endptr, 10);
        if (*endptr)
          continue;

        usage_percent_int = (unsigned int)(usage_percent = round(strtod(val, &endptr)));
        if (endptr == val || strcmp(endptr, "%"))
          continue;

        if (is_gfx_old) {
          process_info->type |= gpu_process_graphical;
          SET_GPUINFO_PROCESS(process_info, gpu_usage, process_info->gpu_usage + usage_percent_int);
        } else if (is_compute_old) {
          process_info->type |= gpu_process_compute;
          SET_GPUINFO_PROCESS(process_info, gpu_usage, process_info->gpu_usage + usage_percent_int);
        } else if (is_dec_old) {
          SET_GPUINFO_PROCESS(process_info, decode_usage, process_info->decode_usage + usage_percent_int);
        } else if (is_enc_old) {
          SET_GPUINFO_PROCESS(process_info, encode_usage, process_info->encode_usage + usage_percent_int);
        }
      } else if (is_gfx_new || is_compute_new || is_dec_new || is_enc_new) {
        char *endptr;
        uint64_t time_spent = strtoull(val, &endptr, 10);
        if (endptr == val || strcmp(endptr, " ns"))
          continue;

        if (is_gfx_new) {
          process_info->type |= gpu_process_graphical;
          SET_GPUINFO_PROCESS(process_info, gfx_engine_used, time_spent);
        } else if (is_compute_new) {
          process_info->type |= gpu_process_compute;
          SET_GPUINFO_PROCESS(process_info, compute_engine_used, time_spent);
        } else if (is_enc_new) {
          SET_GPUINFO_PROCESS(process_info, enc_engine_used, time_spent);
        } else if (is_dec_new) {
          SET_GPUINFO_PROCESS(process_info, dec_engine_used, time_spent);
        }
      }
    }
  }

  // The AMDGPU fdinfo interface in kernels >=5.19 is way nicer; it provides the
  // cumulative GPU engines (e.g., gfx, enc, dec) usage in nanoseconds.
  // Previously, we displayed the usage provided in fdinfo by the kernel/driver
  // which uses an internal update interval. Now, we can compute an accurate
  // busy percentage since the last measurement.
  if (client_id_set) {
    struct amdsmi_process_info_cache *cache_entry;
    struct unique_cache_id ucid = {.client_id = cid, .pid = process_info->pid, .pdev = gpu_info->base.pdev};
    HASH_FIND_PID_PDEV(gpu_info->last_update_process_cache, cid, gpu_info->base.pdev, cache_entry);
    if (cache_entry) {
      uint64_t time_elapsed = nvtop_difftime_u64(cache_entry->last_measurement_tstamp, current_time);
      HASH_DEL(gpu_info->last_update_process_cache, cache_entry);
      if (GPUINFO_PROCESS_FIELD_VALID(process_info, gfx_engine_used) &&
          AMDGPU_CACHE_FIELD_VALID(cache_entry, gfx_engine_used) &&
          // In some rare occasions, the gfx engine usage reported by the driver is lowering (might be a driver bug)
          process_info->gfx_engine_used >= cache_entry->gfx_engine_used &&
          process_info->gfx_engine_used - cache_entry->gfx_engine_used <= time_elapsed) {
        SET_GPUINFO_PROCESS(process_info, gpu_usage,
                            busy_usage_from_time_usage_round(process_info->gfx_engine_used,
                                                             cache_entry->gfx_engine_used, time_elapsed));
      }
      if (GPUINFO_PROCESS_FIELD_VALID(process_info, compute_engine_used) &&
          AMDGPU_CACHE_FIELD_VALID(cache_entry, compute_engine_used) &&
          process_info->compute_engine_used >= cache_entry->compute_engine_used &&
          process_info->compute_engine_used - cache_entry->compute_engine_used <= time_elapsed) {
        unsigned gfx_usage = GPUINFO_PROCESS_FIELD_VALID(process_info, gpu_usage) ? process_info->gpu_usage : 0;
        SET_GPUINFO_PROCESS(process_info, gpu_usage,
                            gfx_usage + busy_usage_from_time_usage_round(process_info->compute_engine_used,
                                                                         cache_entry->compute_engine_used,
                                                                         time_elapsed));
      }
      if (GPUINFO_PROCESS_FIELD_VALID(process_info, dec_engine_used) &&
          AMDGPU_CACHE_FIELD_VALID(cache_entry, dec_engine_used) &&
          process_info->dec_engine_used >= cache_entry->dec_engine_used &&
          process_info->dec_engine_used - cache_entry->dec_engine_used <= time_elapsed) {
        SET_GPUINFO_PROCESS(process_info, decode_usage,
                            busy_usage_from_time_usage_round(process_info->dec_engine_used,
                                                             cache_entry->dec_engine_used, time_elapsed));
      }
      if (GPUINFO_PROCESS_FIELD_VALID(process_info, enc_engine_used) &&
          AMDGPU_CACHE_FIELD_VALID(cache_entry, enc_engine_used) &&
          process_info->enc_engine_used >= cache_entry->enc_engine_used &&
          process_info->enc_engine_used - cache_entry->enc_engine_used <= time_elapsed) {
        SET_GPUINFO_PROCESS(process_info, encode_usage,
                            busy_usage_from_time_usage_round(process_info->enc_engine_used,
                                                             cache_entry->enc_engine_used, time_elapsed));
      }
    } else {
      cache_entry = calloc(1, sizeof(*cache_entry));
      if (!cache_entry)
        goto parse_fdinfo_exit;
      cache_entry->client_id.client_id = cid;
      cache_entry->client_id.pid = process_info->pid;
      cache_entry->client_id.pdev = gpu_info->base.pdev;
    }

    // The UI only shows the decode usage when `encode_decode_shared` is true
    // but amdgpu should only use the encode usage field when it is shared.
    // Lets add both together for good measure.
    if (static_info->encode_decode_shared)
      SET_GPUINFO_PROCESS(process_info, decode_usage, process_info->decode_usage + process_info->encode_usage);

#ifndef NDEBUG
    // We should only process one fdinfo entry per client id per update
    struct amdsmi_process_info_cache *cache_entry_check;
    HASH_FIND_PID_PDEV(gpu_info->current_update_process_cache, cid, gpu_info->base.pdev, cache_entry_check);
    assert(!cache_entry_check && "We should not be processing a client id twice per update");
#endif

    // Store this measurement data
    RESET_ALL(cache_entry->valid);
    if (GPUINFO_PROCESS_FIELD_VALID(process_info, gfx_engine_used))
      SET_AMDGPU_CACHE(cache_entry, gfx_engine_used, process_info->gfx_engine_used);
    if (GPUINFO_PROCESS_FIELD_VALID(process_info, compute_engine_used))
      SET_AMDGPU_CACHE(cache_entry, compute_engine_used, process_info->compute_engine_used);
    if (GPUINFO_PROCESS_FIELD_VALID(process_info, dec_engine_used))
      SET_AMDGPU_CACHE(cache_entry, dec_engine_used, process_info->dec_engine_used);
    if (GPUINFO_PROCESS_FIELD_VALID(process_info, enc_engine_used))
      SET_AMDGPU_CACHE(cache_entry, enc_engine_used, process_info->enc_engine_used);

    cache_entry->last_measurement_tstamp = current_time;
    HASH_ADD_PID_PDEV(gpu_info->current_update_process_cache, cache_entry);
  }

parse_fdinfo_exit:
  return true;
}

static void swap_process_cache_for_next_update(struct gpu_info_amdsmi *gpu_info) {
  // Delete old cache
  struct amdsmi_process_info_cache *cache_entry, *tmp;
  HASH_ITER(hh, gpu_info->last_update_process_cache, cache_entry, tmp) {
    HASH_DEL(gpu_info->last_update_process_cache, cache_entry);
    free(cache_entry);
  }
  // Swap pointers
  gpu_info->last_update_process_cache = gpu_info->current_update_process_cache;
  gpu_info->current_update_process_cache = NULL; // Ready for next update cycle
}

static void gpuinfo_amdsmi_get_running_processes(struct gpu_info *_gpu_info) {
#ifdef HAVE_AMDSMI
  // Implementation using amdsmi_get_gpu_process_list / amdsmi_get_gpu_process_info
  struct gpu_info_amdsmi *gpu_info = container_of(_gpu_info, struct gpu_info_amdsmi, base);
  struct gpuinfo_dynamic_info *dynamic_info = &gpu_info->base.dynamic_info;
  struct gpuinfo_static_info *static_info = &gpu_info->base.static_info;
  amdsmi_proc_info_t *proc_info_list = NULL;
  uint32_t num_procs = 0;
  amdsmi_status_t ret;

  if (!_amdsmi_get_gpu_process_list) {
    local_error_string = "AMDSMI get process list function not loaded";
    goto end; // Or handle error appropriately
  }

  // First call to get the number of processes
  ret = _amdsmi_get_gpu_process_list(gpu_info->processor_handle, NULL, &num_procs);
  if (ret != AMDSMI_STATUS_SUCCESS || num_procs == 0) {
    if (ret != AMDSMI_STATUS_SUCCESS && ret != AMDSMI_STATUS_NOT_FOUND) { // NOT_FOUND is ok if no processes
      local_error_string = _amdsmi_status_code_to_string ? _amdsmi_status_code_to_string(ret) : "AMDSMI Error";
    }
    goto end; // No processes or error
  }

  proc_info_list = calloc(num_procs, sizeof(amdsmi_proc_info_t));
  if (!proc_info_list) {
    local_error_string = "Failed to allocate memory for process list";
    goto end;
  }

  // Second call to get the actual process list
  ret = _amdsmi_get_gpu_process_list(gpu_info->processor_handle, proc_info_list, &num_procs);
  if (ret != AMDSMI_STATUS_SUCCESS) {
    local_error_string = _amdsmi_status_code_to_string ? _amdsmi_status_code_to_string(ret) : "AMDSMI Error";
    free(proc_info_list);
    proc_info_list = NULL;
    goto end;
  }

  // Ensure process array is large enough
  if (gpu_info->base.processes_array_size < num_procs) {
    struct gpu_process *new_processes = realloc(gpu_info->base.processes, num_procs * sizeof(struct gpu_process));
    if (!new_processes) {
      local_error_string = "Failed to reallocate process array";
      free(proc_info_list);
      proc_info_list = NULL;
      goto end;
    }
    gpu_info->base.processes = new_processes;
    gpu_info->base.processes_array_size = num_procs;
  }
  gpu_info->base.processes_count = num_procs;
  memset(gpu_info->base.processes, 0, num_procs * sizeof(struct gpu_process)); // Clear old data

  // Get time difference for usage calculation
  nvtop_time current_time;
  get_current_time(&current_time);
  uint64_t time_elapsed_us = nvtime_diff_us(&dynamic_info->last_measurement_tstamp, &current_time);

  // Iterate through processes and populate gpu_process struct
  for (uint32_t i = 0; i < num_procs; ++i) {
    struct gpu_process *process_info = &gpu_info->base.processes[i];
    process_info->pid = proc_info_list[i].pid;

    // Populate cmdline and username (requires helper functions like get_process_info_linux.c)
    get_process_cmdline(process_info->pid, &process_info->cmdline);
    get_process_user_name(process_info->pid, &process_info->user_name);

    // Set memory usage directly from amdsmi_proc_info_t
    SET_GPUINFO_PROCESS(process_info, gpu_memory_usage, proc_info_list[i].memory_usage.vram_mem);
    if (static_info->vram_size > 0) {
      unsigned percentage =
          (unsigned)((proc_info_list[i].memory_usage.vram_mem * 100) / static_info->vram_size);
      SET_GPUINFO_PROCESS(process_info, gpu_memory_percentage, percentage);
    }

    // Extract engine usage (nanoseconds)
    SET_GPUINFO_PROCESS(process_info, gfx_engine_used, proc_info_list[i].engine_usage.gfx_activity);
    // AMDSMI combines compute into gfx_activity, so compute_engine_used might be 0 or redundant
    // SET_GPUINFO_PROCESS(process_info, compute_engine_used, 0); // Or assign based on specific logic if available
    SET_GPUINFO_PROCESS(process_info, enc_engine_used, proc_info_list[i].engine_usage.mm_enc_activity);
    SET_GPUINFO_PROCESS(process_info, dec_engine_used, proc_info_list[i].engine_usage.mm_dec_activity);
    // Add mm_vce_activity, mm_vcn_activity etc. if needed

    // Calculate usage percentages using cache
    if (time_elapsed_us > 0) {
      struct amdsmi_process_info_cache *cache_entry;
      struct unique_cache_id search_key = {.pid = process_info->pid, .pdev = gpu_info->base.pdev};
      HASH_FIND(hh, gpu_info->last_update_process_cache, &search_key, sizeof(struct unique_cache_id), cache_entry);

      if (cache_entry) {
        if (GPUINFO_PROCESS_FIELD_VALID(process_info, gfx_engine_used) &&
            AMDSMI_CACHE_FIELD_VALID(cache_entry, gfx_engine_ns) &&
            process_info->gfx_engine_used >= cache_entry->gfx_engine_ns &&
            process_info->gfx_engine_used - cache_entry->gfx_engine_ns <= time_elapsed_us * 1000) {
          SET_GPUINFO_PROCESS(process_info, gpu_usage,
                              busy_usage_from_time_usage_round(process_info->gfx_engine_used, cache_entry->gfx_engine_ns,
                                                               time_elapsed_us * 1000));
        }
        if (GPUINFO_PROCESS_FIELD_VALID(process_info, enc_engine_used) &&
            AMDSMI_CACHE_FIELD_VALID(cache_entry, enc_engine_ns) &&
            process_info->enc_engine_used >= cache_entry->enc_engine_ns &&
            process_info->enc_engine_used - cache_entry->enc_engine_ns <= time_elapsed_us * 1000) {
          SET_GPUINFO_PROCESS(process_info, encode_usage,
                              busy_usage_from_time_usage_round(process_info->enc_engine_used,
                                                               cache_entry->enc_engine_ns, time_elapsed_us * 1000));
        }
        if (GPUINFO_PROCESS_FIELD_VALID(process_info, dec_engine_used) &&
            AMDSMI_CACHE_FIELD_VALID(cache_entry, dec_engine_ns) &&
            process_info->dec_engine_used >= cache_entry->dec_engine_ns &&
            process_info->dec_engine_used - cache_entry->dec_engine_ns <= time_elapsed_us * 1000) {
          SET_GPUINFO_PROCESS(process_info, decode_usage,
                              busy_usage_from_time_usage_round(process_info->dec_engine_used,
                                                               cache_entry->dec_engine_ns, time_elapsed_us * 1000));
        }
      }

      // Update current cache
      struct amdsmi_process_info_cache *cache_entry_check;
      struct unique_cache_id current_key = {.pid = process_info->pid, .pdev = gpu_info->base.pdev};
      HASH_FIND(hh, gpu_info->current_update_process_cache, &current_key, sizeof(struct unique_cache_id),
                cache_entry_check);

      if (!cache_entry_check) {
        cache_entry_check = calloc(1, sizeof(struct amdsmi_process_info_cache));
        if (!cache_entry_check) {
          // Handle allocation error
          continue; // Skip this process
        }
        cache_entry_check->client_id.pid = process_info->pid;
        // Need to duplicate pdev string or ensure lifetime? Assume gpu_info->base.pdev is stable
        cache_entry_check->client_id.pdev = gpu_info->base.pdev;
        HASH_ADD(hh, gpu_info->current_update_process_cache, client_id, sizeof(struct unique_cache_id),
                 cache_entry_check);
      }
      if (GPUINFO_PROCESS_FIELD_VALID(process_info, gfx_engine_used))
        SET_AMDSMI_CACHE(cache_entry_check, gfx_engine_ns, process_info->gfx_engine_used);
      // SET_AMDSMI_CACHE(cache_entry_check, compute_engine_ns, process_info->compute_engine_used); // If tracked
      if (GPUINFO_PROCESS_FIELD_VALID(process_info, enc_engine_used))
        SET_AMDSMI_CACHE(cache_entry_check, enc_engine_ns, process_info->enc_engine_used);
      if (GPUINFO_PROCESS_FIELD_VALID(process_info, dec_engine_used))
        SET_AMDSMI_CACHE(cache_entry_check, dec_engine_ns, process_info->dec_engine_used);
      cache_entry_check->last_measurement_tstamp = current_time;
    }
  }

  free(proc_info_list);

#else // Fallback to fdinfo parsing if AMDSMI is not available
  struct gpu_info_amdsmi *gpu_info = container_of(_gpu_info, struct gpu_info_amdsmi, base);
  // Use the generic fdinfo parser
  extract_processinfo_fdinfo(gpu_info->base.pdev, &gpu_info->base.processes_count, &gpu_info->base.processes,
                             &gpu_info->base.processes_array_size,
                             (process_data_parser_func)parse_drm_fdinfo_amd, gpu_info);
#endif // HAVE_AMDSMI

end:
  // Common cleanup/final steps for both paths
  swap_process_cache_for_next_update(container_of(_gpu_info, struct gpu_info_amdsmi, base));
  // Update last measurement timestamp AFTER processing is complete
  get_current_time(&container_of(_gpu_info, struct gpu_info_amdsmi, base)->base.dynamic_info.last_measurement_tstamp);
}
