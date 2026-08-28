# SlimeSeeker 顶层构建入口：定义核心库、CLI、测试、基准和安装导出。
cmake_minimum_required(VERSION 3.24)
project(SlimeSeeker VERSION 1.2.0 LANGUAGES C CXX)

option(SLIMESEEKER_BUILD_TESTS "Build tests" ON)
option(SLIMESEEKER_BUILD_BENCHMARKS "Build benchmarks" ON)
option(SLIMESEEKER_BUILD_SHARED "Build shared library" OFF)
option(SLIMESEEKER_ENABLE_IPO "Enable IPO/LTO for optimized configurations" ON)
option(SLIMESEEKER_ENABLE_CUDA "Build CUDA backend when a CUDA toolkit is available" ON)
option(SLIMESEEKER_BUILD_TUI "Build the interactive terminal UI" ON)
set(SLIMESEEKER_FTXUI_PROVIDER "fetch" CACHE STRING "FTXUI provider: fetch or system")
set_property(CACHE SLIMESEEKER_FTXUI_PROVIDER PROPERTY STRINGS fetch system)
set(SLIMESEEKER_CUDA_ARCHITECTURES "toolchain" CACHE STRING
  "CUDA targets: toolchain, native, release, or a CMake CUDA architecture list")
set_property(CACHE SLIMESEEKER_CUDA_ARCHITECTURES PROPERTY STRINGS toolchain native release)

include(CheckLanguage)
set(SS_CUDA_ENABLED OFF)
if(SLIMESEEKER_ENABLE_CUDA)
  check_language(CUDA)
  if(CMAKE_CUDA_COMPILER)
    enable_language(CUDA)
    set(SS_CUDA_ENABLED ON)
  else()
    message(STATUS "CUDA compiler not found; CUDA backend disabled")
  endif()
endif()

set(SS_CUDA_ARCHITECTURES "")
if(SS_CUDA_ENABLED)
  if(SLIMESEEKER_CUDA_ARCHITECTURES STREQUAL "toolchain")
    message(STATUS "CUDA architectures: toolchain default (${CMAKE_CUDA_ARCHITECTURES})")
  elseif(SLIMESEEKER_CUDA_ARCHITECTURES STREQUAL "native")
    set(SS_CUDA_ARCHITECTURES native)
    message(STATUS "CUDA architectures: native GPU")
  elseif(SLIMESEEKER_CUDA_ARCHITECTURES STREQUAL "release")
    if(CMAKE_CUDA_COMPILER_VERSION VERSION_LESS 13.0)
      message(FATAL_ERROR
        "SLIMESEEKER_CUDA_ARCHITECTURES=release requires CUDA 13.0 or newer")
    endif()
    # 发布包携带各代真实 SASS，并保留最高架构 PTX 供后续兼容设备由驱动 JIT。
    set(SS_CUDA_ARCHITECTURES
      75-real 80-real 86-real 89-real 90-real 100-real 120-real 120-virtual)
    message(STATUS "CUDA architectures: release fat binary (${SS_CUDA_ARCHITECTURES})")
  else()
    set(SS_CUDA_ARCHITECTURES "${SLIMESEEKER_CUDA_ARCHITECTURES}")
    message(STATUS "CUDA architectures: explicit (${SS_CUDA_ARCHITECTURES})")
  endif()
  if(SS_CUDA_ARCHITECTURES)
    # 静态库和最终可执行文件都会执行 CUDA device-link；两层必须使用同一架构集合，
    # 否则最终链接可能按工具链默认值裁掉库中已经生成的其他 code object。
    set(CMAKE_CUDA_ARCHITECTURES "${SS_CUDA_ARCHITECTURES}")
  endif()
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 源码与公共头文件包含中文说明，MSVC 需显式按 UTF-8 解码以避免依赖系统代码页。
if(MSVC)
  add_compile_options(/utf-8)
endif()

# IPO 不是所有编译器/链接器组合都支持；探测失败时保持普通优化构建，而不是阻断配置。
set(SS_IPO_AVAILABLE OFF)
if(SLIMESEEKER_ENABLE_IPO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT SS_IPO_AVAILABLE OUTPUT SS_IPO_ERROR LANGUAGES CXX)
  if(NOT SS_IPO_AVAILABLE)
    message(WARNING "IPO/LTO is unavailable: ${SS_IPO_ERROR}")
  endif()
endif()

function(ss_enable_ipo target_name)
  if(SS_IPO_AVAILABLE)
    set_property(TARGET ${target_name} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set_property(TARGET ${target_name} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
  endif()
endfunction()

set(SS_LIBRARY_TYPE STATIC)
if(SLIMESEEKER_BUILD_SHARED)
  set(SS_LIBRARY_TYPE SHARED)
endif()

add_library(slimeseeker ${SS_LIBRARY_TYPE}
  src/core/domain.cpp
  src/engine/search.cpp
  src/engine/search_dispatch.cpp
  src/api/c_api.cpp
  src/backends/scalar.cpp
  src/backends/dispatch.cpp)
if(SS_CUDA_ENABLED)
  target_sources(slimeseeker PRIVATE src/backends/cuda.cu)
  target_compile_definitions(slimeseeker PRIVATE SS_HAS_CUDA=1)
  target_compile_features(slimeseeker PRIVATE cuda_std_17)
  set_target_properties(slimeseeker PROPERTIES
    CUDA_RUNTIME_LIBRARY Static)
  if(SS_CUDA_ARCHITECTURES)
    set_target_properties(slimeseeker PROPERTIES
      CUDA_ARCHITECTURES "${SS_CUDA_ARCHITECTURES}")
  endif()
else()
  target_sources(slimeseeker PRIVATE src/backends/cuda_stub.cpp)
endif()
add_library(SlimeSeeker::slimeseeker ALIAS slimeseeker)
ss_enable_ipo(slimeseeker)
target_include_directories(slimeseeker
  PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
         $<INSTALL_INTERFACE:include>
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_definitions(slimeseeker PRIVATE SLIMESEEKER_BUILDING_LIBRARY=1)
if(SLIMESEEKER_BUILD_SHARED)
  target_compile_definitions(slimeseeker PUBLIC SLIMESEEKER_SHARED=1)
endif()
if(MSVC)
  target_compile_options(slimeseeker PRIVATE /W4 /permissive-)
else()
  target_compile_options(slimeseeker PRIVATE -Wall -Wextra -Wpedantic)
endif()

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" SS_PROCESSOR)
if(SS_PROCESSOR MATCHES "^(x86_64|amd64|x64)$")
  target_sources(slimeseeker PRIVATE src/backends/avx2.cpp)
  if(MSVC)
    set_source_files_properties(src/backends/avx2.cpp PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
  else()
    set_source_files_properties(src/backends/avx2.cpp PROPERTIES COMPILE_OPTIONS "-mavx2")
  endif()
  target_compile_definitions(slimeseeker PRIVATE SS_HAS_AVX2_OBJECT=1)
elseif(SS_PROCESSOR MATCHES "^(aarch64|arm64)$")
  target_sources(slimeseeker PRIVATE src/backends/neon.cpp)
  target_compile_definitions(slimeseeker PRIVATE SS_HAS_NEON_OBJECT=1)
endif()

find_package(Threads REQUIRED)
target_link_libraries(slimeseeker PRIVATE Threads::Threads)

add_library(slimeseeker_worldgen STATIC
  src/worldgen/worldgen.cpp
  src/worldgen/cubiomes_bridge.c
  src/worldgen/third_party/cubiomes/biomenoise.c
  src/worldgen/third_party/cubiomes/noise.c
  src/worldgen/third_party/cubiomes/layers.c
  src/worldgen/third_party/cubiomes/biomes.c)
target_include_directories(slimeseeker_worldgen PRIVATE src)

add_executable(slimeseeker_cli
  apps/cli/main.cpp
  apps/cli/cli.cpp
  apps/cli/biome_score.cpp)
target_include_directories(slimeseeker_cli PRIVATE apps/cli src)
if(SLIMESEEKER_BUILD_TUI)
  if(SLIMESEEKER_FTXUI_PROVIDER STREQUAL "system")
    find_package(ftxui 7.0 CONFIG REQUIRED)
  elseif(SLIMESEEKER_FTXUI_PROVIDER STREQUAL "fetch")
    include(FetchContent)
    set(FTXUI_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(FTXUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(FTXUI_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(ftxui
      URL https://codeload.github.com/ArthurSonzogni/FTXUI/tar.gz/refs/tags/v7.0.3
      URL_HASH SHA256=e7c62ffe19009759821b4f0f8df7f2a6fb83784c3a9f1477d81f56d3ee723c88
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(ftxui)
  else()
    message(FATAL_ERROR "SLIMESEEKER_FTXUI_PROVIDER must be fetch or system")
  endif()
  target_sources(slimeseeker_cli PRIVATE apps/cli/tui.cpp apps/cli/tui_model.cpp)
  target_link_libraries(slimeseeker_cli PRIVATE ftxui::screen ftxui::dom ftxui::component)
else()
  target_sources(slimeseeker_cli PRIVATE apps/cli/tui_stub.cpp)
endif()
ss_enable_ipo(slimeseeker_cli)
set_target_properties(slimeseeker_cli PROPERTIES OUTPUT_NAME slimeseeker)
if(MSVC)
  target_compile_options(slimeseeker_cli PRIVATE /utf-8)
endif()
target_link_libraries(slimeseeker_cli PRIVATE SlimeSeeker::slimeseeker slimeseeker_worldgen)

include(GNUInstallDirs)
install(TARGETS slimeseeker slimeseeker_cli EXPORT SlimeSeekerTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT SlimeSeekerTargets NAMESPACE SlimeSeeker:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SlimeSeeker)
include(CMakePackageConfigHelpers)
configure_package_config_file(cmake/SlimeSeekerConfig.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/SlimeSeekerConfig.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SlimeSeeker)
write_basic_package_version_file(${CMAKE_CURRENT_BINARY_DIR}/SlimeSeekerConfigVersion.cmake
  VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/SlimeSeekerConfig.cmake
              ${CMAKE_CURRENT_BINARY_DIR}/SlimeSeekerConfigVersion.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SlimeSeeker)
if(SLIMESEEKER_BUILD_TUI)
  install(FILES licenses/FTXUI.txt
    DESTINATION ${CMAKE_INSTALL_DATADIR}/slimeseeker/licenses)
endif()
install(FILES licenses/Cubiomes.txt
  DESTINATION ${CMAKE_INSTALL_DATADIR}/slimeseeker/licenses)

if(SLIMESEEKER_BUILD_TESTS)
  enable_testing()
  add_executable(slimeseeker_unit_tests tests/unit/tests.cpp)
  ss_enable_ipo(slimeseeker_unit_tests)
  target_include_directories(slimeseeker_unit_tests PRIVATE src)
  target_link_libraries(slimeseeker_unit_tests PRIVATE SlimeSeeker::slimeseeker)
  add_test(NAME unit_tests COMMAND slimeseeker_unit_tests)
  add_test(NAME cli_help COMMAND slimeseeker_cli --help)
  add_executable(slimeseeker_regression tests/regression/golden.cpp)
  ss_enable_ipo(slimeseeker_regression)
  target_link_libraries(slimeseeker_regression PRIVATE SlimeSeeker::slimeseeker)
  add_test(NAME full_golden_regression COMMAND slimeseeker_regression)
  add_executable(slimeseeker_c_api_smoke tests/integration/c_api_smoke.c)
  set_target_properties(slimeseeker_c_api_smoke PROPERTIES LINKER_LANGUAGE CXX)
  target_link_libraries(slimeseeker_c_api_smoke PRIVATE SlimeSeeker::slimeseeker)
  add_test(NAME c_api_smoke COMMAND slimeseeker_c_api_smoke)
  add_executable(slimeseeker_worldgen_tests tests/unit/worldgen_tests.cpp)
  target_include_directories(slimeseeker_worldgen_tests PRIVATE src)
  target_link_libraries(slimeseeker_worldgen_tests PRIVATE slimeseeker_worldgen)
  add_test(NAME worldgen_tests COMMAND slimeseeker_worldgen_tests)
  add_test(NAME biome_score_cli
    COMMAND ${CMAKE_COMMAND}
      -DSLIMESEEKER_CLI=$<TARGET_FILE:slimeseeker_cli>
      -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/biome_score.cmake)
  if(SLIMESEEKER_BUILD_TUI)
    add_executable(slimeseeker_tui_model_tests
      tests/unit/tui_model_tests.cpp apps/cli/tui_model.cpp)
    target_include_directories(slimeseeker_tui_model_tests PRIVATE apps/cli src)
    target_link_libraries(slimeseeker_tui_model_tests PRIVATE SlimeSeeker::slimeseeker)
    add_test(NAME tui_model_tests COMMAND slimeseeker_tui_model_tests)
    add_test(NAME tui_requires_terminal
      COMMAND ${CMAKE_COMMAND}
        -DSLIMESEEKER_CLI=$<TARGET_FILE:slimeseeker_cli>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/tui_noninteractive.cmake)
  endif()
endif()

if(SLIMESEEKER_BUILD_BENCHMARKS)
  add_executable(slimeseeker_bench benchmarks/benchmark.cpp)
  ss_enable_ipo(slimeseeker_bench)
  target_include_directories(slimeseeker_bench PRIVATE src)
  target_link_libraries(slimeseeker_bench PRIVATE SlimeSeeker::slimeseeker)
endif()
