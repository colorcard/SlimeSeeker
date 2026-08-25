# SlimeSeeker 顶层构建入口：定义核心库、CLI、测试、基准和安装导出。
cmake_minimum_required(VERSION 3.24)
project(SlimeSeeker VERSION 1.0.0 LANGUAGES C CXX)

option(SLIMESEEKER_BUILD_TESTS "Build tests" ON)
option(SLIMESEEKER_BUILD_BENCHMARKS "Build benchmarks" ON)
option(SLIMESEEKER_BUILD_SHARED "Build shared library" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 源码与公共头文件包含中文说明，MSVC 需显式按 UTF-8 解码以避免依赖系统代码页。
if(MSVC)
  add_compile_options(/utf-8)
endif()

set(SS_LIBRARY_TYPE STATIC)
if(SLIMESEEKER_BUILD_SHARED)
  set(SS_LIBRARY_TYPE SHARED)
endif()

add_library(slimeseeker ${SS_LIBRARY_TYPE}
  src/core/domain.cpp
  src/engine/search.cpp
  src/api/c_api.cpp
  src/backends/scalar.cpp
  src/backends/dispatch.cpp)
add_library(SlimeSeeker::slimeseeker ALIAS slimeseeker)
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

add_executable(slimeseeker_cli apps/cli/main.cpp)
set_target_properties(slimeseeker_cli PROPERTIES OUTPUT_NAME slimeseeker)
target_link_libraries(slimeseeker_cli PRIVATE SlimeSeeker::slimeseeker)

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

if(SLIMESEEKER_BUILD_TESTS)
  enable_testing()
  add_executable(slimeseeker_unit_tests tests/unit/tests.cpp)
  target_include_directories(slimeseeker_unit_tests PRIVATE src)
  target_link_libraries(slimeseeker_unit_tests PRIVATE SlimeSeeker::slimeseeker)
  add_test(NAME unit_tests COMMAND slimeseeker_unit_tests)
  add_test(NAME cli_help COMMAND slimeseeker_cli --help)
  add_executable(slimeseeker_regression tests/regression/golden.cpp)
  target_link_libraries(slimeseeker_regression PRIVATE SlimeSeeker::slimeseeker)
  add_test(NAME full_golden_regression COMMAND slimeseeker_regression)
  add_executable(slimeseeker_c_api_smoke tests/integration/c_api_smoke.c)
  set_target_properties(slimeseeker_c_api_smoke PROPERTIES LINKER_LANGUAGE CXX)
  target_link_libraries(slimeseeker_c_api_smoke PRIVATE SlimeSeeker::slimeseeker)
  add_test(NAME c_api_smoke COMMAND slimeseeker_c_api_smoke)
endif()

if(SLIMESEEKER_BUILD_BENCHMARKS)
  add_executable(slimeseeker_bench benchmarks/benchmark.cpp)
  target_include_directories(slimeseeker_bench PRIVATE src)
  target_link_libraries(slimeseeker_bench PRIVATE SlimeSeeker::slimeseeker)
endif()
