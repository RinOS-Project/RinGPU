cmake_minimum_required(VERSION 3.16)
project(RinGPU VERSION 1.0.0 LANGUAGES C)

option(RINGPU_BUILD_SHARED "Build RinGPU as a shared library" OFF)
option(RINGPU_BUILD_TESTS "Build RinGPU host tests" ON)
option(RINGPU_BUILD_DIFFERENTIAL_TESTS "Build RinGPU differential support tests" OFF)
option(RINGPU_BUILD_BENCHMARKS "Build optional RinGPU host benchmarks" OFF)
option(RINGPU_BUILD_RINOS_IPC_ADAPTER
       "Build the optional RinOS SDK cross-process IPC adapter" ON)
option(RINGPU_BUILD_PRESENTATION "Build the RinGPU presentation runtime" ON)
option(RINGPU_BUILD_SOFTWARE_BACKEND
       "Build the generic CPU software backend in the RinGPU archive" ON)
include(GNUInstallDirs)

set(RINGPU_SOURCES
    src/command/commands.c
    src/command/draw.c
    src/command/record.c
    src/core/core.c
    src/core/object_lifetime.c
    src/core/object_table.c
    src/platform/thread.c
    src/runtime.c
    src/validation/diagnostics.c
    src/validation/graphics_layout.c
    src/validation/pipeline.c
    src/validation/resource.c
    src/validation/shader.c
    src/sync/barriers.c
    src/sync/fences.c
    src/device/device_runtime.c
    src/device/fault_policy.c
    src/memory/memory_runtime.c
    src/presentation/present.c
    src/presentation/render_pass.c
    src/pipeline/pipelines.c
    src/queue/submit.c
    src/pipeline/graphics.c
    src/resource/resources.c
    src/query/queries.c
    src/shader/modules.c
    src/shader/spirv_frontend.c
    src/capability/cross_process_capability.c)

if(RINGPU_BUILD_PRESENTATION)
    list(APPEND RINGPU_SOURCES src/presentation/presentation.c)
endif()

if(RINGPU_BUILD_SOFTWARE_BACKEND)
    list(APPEND RINGPU_SOURCES src/software/software_backend.c)
endif()

set(RINGPU_ENABLE_PRESENTATION 0)
if(RINGPU_BUILD_PRESENTATION)
    set(RINGPU_ENABLE_PRESENTATION 1)
endif()

if(RINGPU_BUILD_SHARED)
    add_library(RinGPU SHARED ${RINGPU_SOURCES})
else()
    add_library(RinGPU STATIC ${RINGPU_SOURCES})
endif()
add_library(RinGPU::RinGPU ALIAS RinGPU)

if(WIN32 AND RINGPU_BUILD_SHARED)
    set_target_properties(RinGPU PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

if(RINGPU_BUILD_RINOS_IPC_ADAPTER)
    add_library(RinGPUIpc STATIC
        src/capability/cross_process_capability_ipc.c)
    add_library(RinGPU::RinGPUIpc ALIAS RinGPUIpc)
    target_compile_features(RinGPUIpc PUBLIC c_std_11)
    target_include_directories(RinGPUIpc
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
               $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../RinOS-SDK/include>
               $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
    target_link_libraries(RinGPUIpc PUBLIC RinGPU::RinGPU)
    set_target_properties(RinGPUIpc PROPERTIES POSITION_INDEPENDENT_CODE ON)
    if(MSVC)
        target_compile_options(RinGPUIpc PRIVATE /W4)
    else()
        target_compile_options(RinGPUIpc PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    install(TARGETS RinGPUIpc EXPORT RinGPUTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

target_compile_features(RinGPU PUBLIC c_std_11)
target_compile_definitions(RinGPU PUBLIC
    RINGPU_ENABLE_PRESENTATION=$<BOOL:${RINGPU_BUILD_PRESENTATION}>)
target_compile_definitions(RinGPU PRIVATE
    RINGPU_BUILD_SOFTWARE_BACKEND=$<BOOL:${RINGPU_BUILD_SOFTWARE_BACKEND}>)
target_include_directories(RinGPU
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../rinresource/include>
           $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../RinOS-SDK/include>
           $<INSTALL_INTERFACE:include>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/core
            ${CMAKE_CURRENT_SOURCE_DIR}/src/validation
            ${CMAKE_CURRENT_SOURCE_DIR}/src/device
            ${CMAKE_CURRENT_SOURCE_DIR}/src/memory
            ${CMAKE_CURRENT_SOURCE_DIR}/src/software
            ${CMAKE_CURRENT_SOURCE_DIR}/src/capability)
if(MSVC)
    target_compile_options(RinGPU PRIVATE /W4)
else()
    target_compile_options(RinGPU PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS RinGPU EXPORT RinGPUTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT RinGPUTargets
        FILE RinGPUTargets.cmake
        NAMESPACE RinGPU::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RinGPU)

configure_file(ringpu.pc.in ringpu.pc @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ringpu.pc
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

if(RINGPU_BUILD_TESTS)
    enable_testing()
    function(ringpu_add_test target test_name source)
        add_executable(${target} ${source})
        target_link_libraries(${target} PRIVATE RinGPU::RinGPU)
        add_test(NAME ${test_name} COMMAND ${target})
    endfunction()

    ringpu_add_test(ringpu-abi-test ringpu-abi tests/ringpu_abi_test.c)
    add_executable(ringpu-platform-thread-test
                   tests/ringpu_platform_thread_test.c
                   src/platform/thread.c)
    target_compile_features(ringpu-platform-thread-test PRIVATE c_std_11)
    target_include_directories(ringpu-platform-thread-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/platform)
    if(NOT WIN32)
        target_link_libraries(ringpu-platform-thread-test PRIVATE
                              Threads::Threads)
    endif()
    add_test(NAME ringpu-platform-thread
             COMMAND ringpu-platform-thread-test)
    ringpu_add_test(ringpu-shader-test ringpu-shader
                    tests/ringpu_shader_test.c)
    ringpu_add_test(ringpu-shader-resource-test ringpu-shader-resource
                    tests/ringpu_shader_resource_test.c)
    ringpu_add_test(ringpu-result-mapping-test ringpu-result-mapping
                    tests/ringpu_result_mapping_test.c)
    if(RINGPU_BUILD_RINOS_IPC_ADAPTER)
        add_executable(ringpu-cross-process-ipc-test
                       tests/ringpu_cross_process_ipc_test.c)
        target_link_libraries(ringpu-cross-process-ipc-test
                              PRIVATE RinGPU::RinGPUIpc)
        add_test(NAME ringpu-cross-process-ipc
                 COMMAND ringpu-cross-process-ipc-test)
    endif()

    if(RINGPU_BUILD_SOFTWARE_BACKEND)
        ringpu_add_test(ringpu-runtime-test ringpu-runtime
                        tests/ringpu_runtime_test.c)
        ringpu_add_test(ringpu-resource-requirements-test
                        ringpu-resource-requirements
                        tests/ringpu_resource_requirements_test.c)
        ringpu_add_test(ringpu-primitive-restart-test ringpu-primitive-restart
                        tests/ringpu_primitive_restart_test.c)
        ringpu_add_test(ringpu-srgb-test ringpu-srgb
                        tests/ringpu_srgb_test.c)
        ringpu_add_test(ringpu-query-test ringpu-query
                        tests/ringpu_query_test.c)
        ringpu_add_test(ringpu-ownership-test ringpu-ownership
                        tests/ringpu_ownership_test.c)
        ringpu_add_test(ringpu-resolve-test ringpu-resolve
                        tests/ringpu_resolve_test.c)
        ringpu_add_test(ringpu-shared-test ringpu-shared
                        tests/ringpu_shared_test.c)
        ringpu_add_test(ringpu-storage-image-test ringpu-storage-image
                        tests/ringpu_storage_image_test.c)
        ringpu_add_test(ringpu-compressed-test ringpu-compressed
                        tests/ringpu_compressed_test.c)
        ringpu_add_test(ringpu-memory-binding-test ringpu-memory-binding
                        tests/ringpu_memory_binding_test.c)
        ringpu_add_test(ringpu-software-memory-suballocator-test
                        ringpu-software-memory-suballocator
                        tests/ringpu_software_memory_suballocator_test.c)
        ringpu_add_test(ringpu-subresource-range-test ringpu-subresource-range
                        tests/ringpu_subresource_range_test.c)
    else()
        message(STATUS
            "Software-backend conformance tests are omitted because RINGPU_BUILD_SOFTWARE_BACKEND is OFF")
    endif()
endif()

if(RINGPU_BUILD_DIFFERENTIAL_TESTS AND RINGPU_BUILD_SOFTWARE_BACKEND)
    add_executable(ringpu-differential-test
                   tests/differential/differential_test.c
                   tests/differential/differential.c)
    target_include_directories(ringpu-differential-test
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/differential)
    target_link_libraries(ringpu-differential-test PRIVATE RinGPU::RinGPU)
    add_test(NAME ringpu-differential COMMAND ringpu-differential-test)
    add_executable(ringpu-determinism-test
                   tests/differential/determinism_test.c
                   tests/differential/differential.c)
    target_include_directories(ringpu-determinism-test
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/differential)
    target_link_libraries(ringpu-determinism-test PRIVATE RinGPU::RinGPU)
    add_test(NAME ringpu-determinism COMMAND ringpu-determinism-test)
elseif(RINGPU_BUILD_DIFFERENTIAL_TESTS)
    message(STATUS
        "Differential tests are omitted because they require the software backend")
endif()

if(RINGPU_BUILD_BENCHMARKS)
    if(NOT RINGPU_BUILD_SOFTWARE_BACKEND)
        message(FATAL_ERROR
            "RINGPU_BUILD_BENCHMARKS requires RINGPU_BUILD_SOFTWARE_BACKEND")
    endif()
    add_executable(ringpu-host-benchmark
                   benchmarks/host_runtime_benchmark.c)
    target_link_libraries(ringpu-host-benchmark PRIVATE RinGPU::RinGPU)
    if(RINGPU_BUILD_TESTS)
        add_test(NAME ringpu-host-benchmark-smoke
                 COMMAND ringpu-host-benchmark --iterations 5 --warmup 1)
    endif()
endif()
