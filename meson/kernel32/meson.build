# The kernel32 build graph links this archive as a dependency.  Keep RinGPU's
# implementation file ownership in this submodule instead of listing its
# sources in the root kernel build.
ringpu_kernel32 = static_library(
  'ringpu-kernel32',
  files(
    '../../src/capability/cross_process_capability.c',
    '../../src/device/device_runtime.c',
    '../../src/memory/memory_runtime.c',
    '../../src/presentation/presentation.c',
    '../../src/validation/diagnostics.c',
  ),
  c_args: ringpu_kernel32_c_args,
  pic: false,
  install: false,
)
