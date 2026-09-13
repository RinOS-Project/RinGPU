project('RinGPU', 'c', version: '1.0.0', default_options: ['c_std=c11', 'warning_level=3'])

inc = include_directories('include', 'src/core', 'src/validation', 'src/device',
                          'src/memory', 'src/software', 'src/capability')
ringpu_sources = files(
  'src/command/record.c',
  'src/core/core.c',
  'src/core/object_table.c',
  'src/runtime.c',
  'src/validation/diagnostics.c',
  'src/validation/graphics_layout.c',
  'src/validation/pipeline.c',
  'src/validation/resource.c',
  'src/validation/shader.c',
  'src/software/software_backend.c',
  'src/sync/barriers.c',
  'src/sync/fences.c',
  'src/device/device_runtime.c',
  'src/device/fault_policy.c',
  'src/memory/memory_runtime.c',
  'src/capability/cross_process_capability.c',
)

ringpu = library('RinGPU', ringpu_sources, include_directories: inc,
                 install: true,
                 version: '1.0.0')
ringpu_dep = declare_dependency(link_with: ringpu,
                                include_directories: include_directories('include'))
install_subdir('include/ringpu', install_dir: get_option('includedir'))

if get_option('buildtype') != 'plain'
  test('ringpu-abi', executable('ringpu-abi-test', 'tests/ringpu_abi_test.c',
                                dependencies: ringpu_dep))
  test('ringpu-shader', executable('ringpu-shader-test', 'tests/ringpu_shader_test.c',
                                   dependencies: ringpu_dep))
  test('ringpu-runtime', executable('ringpu-runtime-test', 'tests/ringpu_runtime_test.c',
                                   dependencies: ringpu_dep))
  if get_option('build_differential_tests')
    test('ringpu-differential', executable('ringpu-differential-test',
                                          'tests/differential/differential_test.c',
                                          'tests/differential/differential.c',
                                          include_directories: inc,
                                          dependencies: ringpu_dep))
  endif
endif
