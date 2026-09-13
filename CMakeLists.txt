cmake_minimum_required(VERSION 3.16)
project(RinGPU VERSION 1.0.0 LANGUAGES C)

option(RINGPU_BUILD_SHARED "Build RinGPU as a shared library" OFF)
option(RINGPU_BUILD_TESTS "Build RinGPU host tests" ON)
option(RINGPU_BUILD_DIFFERENTIAL_TESTS "Build RinGPU differential support tests" OFF)

set(RINGPU_SOURCES
    src/core/core.c
    src/runtime.c
    src/validation/diagnostics.c
    src/validation/graphics_layout.c
    src/validation/pipeline.c
    src/validation/resource.c
    src/validation/shader.c
    src/software/software_backend.c
    src/device/device_runtime.c
    src/device/fault_policy.c
    src/memory/memory_runtime.c
    src/capability/cross_process_capability.c)

if(RINGPU_BUILD_SHARED)
    add_library(RinGPU SHARED ${RINGPU_SOURCES})
else()
    add_library(RinGPU STATIC ${RINGPU_SOURCES})
endif()
add_library(RinGPU::RinGPU ALIAS RinGPU)

target_compile_features(RinGPU PUBLIC c_std_11)
target_include_directories(RinGPU
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
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

include(GNUInstallDirs)
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
    add_executable(ringpu-abi-test tests/ringpu_abi_test.c)
    target_link_libraries(ringpu-abi-test PRIVATE RinGPU::RinGPU)
    add_test(NAME ringpu-abi COMMAND ringpu-abi-test)
    add_executable(ringpu-shader-test tests/ringpu_shader_test.c)
    target_link_libraries(ringpu-shader-test PRIVATE RinGPU::RinGPU)
    add_test(NAME ringpu-shader COMMAND ringpu-shader-test)
    add_executable(ringpu-runtime-test tests/ringpu_runtime_test.c)
    target_link_libraries(ringpu-runtime-test PRIVATE RinGPU::RinGPU)
    add_test(NAME ringpu-runtime COMMAND ringpu-runtime-test)
endif()

if(RINGPU_BUILD_DIFFERENTIAL_TESTS)
    add_executable(ringpu-differential-test
                   tests/differential/differential_test.c
                   tests/differential/differential.c)
    target_include_directories(ringpu-differential-test
                               PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests/differential)
    target_link_libraries(ringpu-differential-test PRIVATE RinGPU::RinGPU)
    add_test(NAME ringpu-differential COMMAND ringpu-differential-test)
endif()
