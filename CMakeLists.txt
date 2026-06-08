cmake_minimum_required(VERSION 3.20)
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

# Ninja parses MSVC /showIncludes output to track header dependencies. Set this
# before project() so CMake's compiler-prefix detection also sees English output.
set(ENV{VSLANG} "1033")

project(FlashDEG VERSION 1.0.0 LANGUAGES C CXX)

include(GNUInstallDirs)

find_package(Git QUIET)
set(FLASHDEG_GIT_REV "unknown")
if(GIT_FOUND)
  execute_process(
    COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE FLASHDEG_GIT_REV
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  execute_process(
    COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=all
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE _flashdeg_git_status
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  if(_flashdeg_git_status)
    string(APPEND FLASHDEG_GIT_REV "-dirty")
  endif()
  unset(_flashdeg_git_status)
endif()
if(NOT FLASHDEG_GIT_REV)
  set(FLASHDEG_GIT_REV "unknown")
endif()
string(TIMESTAMP FLASHDEG_BUILD_DATE "%Y-%m-%d" UTC)

option(FLASHDEG_BUILD_TESTS "Build FlashDEG tests" ON)
option(FLASHDEG_USE_EIGEN "Prefer Eigen for numerical linear algebra when wired" ON)
option(FLASHDEG_USE_BOOST_MATH "Prefer Boost.Math for special functions when wired" ON)
option(FLASHDEG_USE_BLAS "Prefer BLAS/LAPACK acceleration when wired" ON)
option(FLASHDEG_USE_SCIPY_LBFGSB
       "Use SciPy's vendored L-BFGS-B C translation when BLAS is available"
       ON)
option(FLASHDEG_ENABLE_DEV_OPTIONS
       "Enable developer-only CLI options and oracle/debug outputs"
       OFF)
option(FLASHDEG_ALLOW_NO_DEPS_BUILD
       "Allow dependency-free fallback builds for development/testing"
       OFF)
set(FLASHDEG_DEFAULT_COMPAT_MODE "deseq2-r" CACHE STRING
    "Default CLI compatibility mode: pydeseq2 or deseq2-r")
set_property(CACHE FLASHDEG_DEFAULT_COMPAT_MODE PROPERTY STRINGS
  "pydeseq2" "deseq2-r")
if(NOT FLASHDEG_DEFAULT_COMPAT_MODE STREQUAL "pydeseq2" AND
   NOT FLASHDEG_DEFAULT_COMPAT_MODE STREQUAL "deseq2-r")
  message(FATAL_ERROR
    "FLASHDEG_DEFAULT_COMPAT_MODE must be 'pydeseq2' or 'deseq2-r'")
endif()

if(NOT FLASHDEG_USE_EIGEN AND NOT FLASHDEG_USE_BOOST_MATH AND
   NOT FLASHDEG_USE_BLAS AND NOT FLASHDEG_USE_SCIPY_LBFGSB AND
   NOT FLASHDEG_ALLOW_NO_DEPS_BUILD)
  message(FATAL_ERROR
    "Dependency-free builds are disabled by default. Install Eigen, Boost.Math, "
    "and BLAS/LAPACK, or explicitly configure a developer fallback build with "
    "FLASHDEG_ALLOW_NO_DEPS_BUILD=ON and all FLASHDEG_USE_* backend options OFF.")
endif()

if(FLASHDEG_USE_EIGEN)
  find_package(Eigen3 CONFIG REQUIRED)
  message(STATUS "FlashDEG: using Eigen ${Eigen3_VERSION}")
endif()

if(FLASHDEG_USE_BOOST_MATH)
  find_package(Boost REQUIRED)
  message(STATUS "FlashDEG: using Boost ${Boost_VERSION}")
endif()

if(FLASHDEG_USE_BLAS)
  find_package(OpenBLAS CONFIG QUIET)
  if(OpenBLAS_FOUND)
    message(STATUS "FlashDEG: using OpenBLAS")
  else()
    find_package(BLAS QUIET)
    find_package(LAPACK QUIET)
    if(BLAS_FOUND AND LAPACK_FOUND)
      message(STATUS "FlashDEG: using BLAS/LAPACK")
    else()
      message(FATAL_ERROR
        "FLASHDEG_USE_BLAS=ON requires OpenBLAS or BLAS/LAPACK. Install a "
        "BLAS/LAPACK backend or explicitly configure a developer fallback build "
        "with FLASHDEG_ALLOW_NO_DEPS_BUILD=ON and FLASHDEG_USE_BLAS=OFF.")
    endif()
  endif()
endif()

if(FLASHDEG_USE_SCIPY_LBFGSB)
  if(NOT OpenBLAS_FOUND AND NOT BLAS_FOUND)
    message(FATAL_ERROR
      "FLASHDEG_USE_SCIPY_LBFGSB=ON requires OpenBLAS or BLAS")
  endif()

  include(CheckCSourceCompiles)
  set(_flashdeg_required_libraries_saved "${CMAKE_REQUIRED_LIBRARIES}")
  if(OpenBLAS_FOUND)
    set(CMAKE_REQUIRED_LIBRARIES OpenBLAS::OpenBLAS)
  else()
    set(CMAKE_REQUIRED_LIBRARIES ${BLAS_LIBRARIES})
    if(LAPACK_FOUND)
      list(APPEND CMAKE_REQUIRED_LIBRARIES ${LAPACK_LIBRARIES})
    endif()
  endif()
  unset(FLASHDEG_HAVE_SYSTEM_DPOTRF CACHE)
  unset(FLASHDEG_HAVE_SYSTEM_DTRTRS CACHE)
  check_c_source_compiles(
    "extern void dpotrf_(char*, int*, double*, int*, int*);
     int main(void) {
       char u = 'U';
       int n = 0;
       int lda = 1;
       int info = 0;
       double a[1] = {0.0};
       dpotrf_(&u, &n, a, &lda, &info);
       return 0;
     }"
    FLASHDEG_HAVE_SYSTEM_DPOTRF)
  check_c_source_compiles(
    "extern void dtrtrs_(char*, char*, char*, int*, int*, double*, int*, double*, int*, int*);
     int main(void) {
       char u = 'U';
       char t = 'N';
       char d = 'N';
       int n = 0;
       int nrhs = 0;
       int lda = 1;
       int ldb = 1;
       int info = 0;
       double a[1] = {1.0};
       double b[1] = {0.0};
       dtrtrs_(&u, &t, &d, &n, &nrhs, a, &lda, b, &ldb, &info);
       return 0;
     }"
    FLASHDEG_HAVE_SYSTEM_DTRTRS)
  set(CMAKE_REQUIRED_LIBRARIES "${_flashdeg_required_libraries_saved}")
  unset(_flashdeg_required_libraries_saved)

  set(_scipy_lbfgsb_sources
    third_party/scipy_lbfgsb/__lbfgsb.c
  )
  if(FLASHDEG_HAVE_SYSTEM_DPOTRF AND FLASHDEG_HAVE_SYSTEM_DTRTRS)
    message(STATUS "FlashDEG: using system LAPACK dpotrf_/dtrtrs_ for SciPy L-BFGS-B")
  else()
    list(APPEND _scipy_lbfgsb_sources third_party/scipy_lbfgsb/lapack_shims.cpp)
    message(STATUS "FlashDEG: bundling LAPACK shims for SciPy L-BFGS-B")
  endif()

  add_library(scipy_lbfgsb STATIC ${_scipy_lbfgsb_sources})
  unset(_scipy_lbfgsb_sources)
  set_target_properties(scipy_lbfgsb PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
  if(OpenBLAS_FOUND)
    target_link_libraries(scipy_lbfgsb PUBLIC OpenBLAS::OpenBLAS)
  else()
    target_link_libraries(scipy_lbfgsb PUBLIC ${BLAS_LIBRARIES})
    if(LAPACK_FOUND)
      target_link_libraries(scipy_lbfgsb PUBLIC ${LAPACK_LIBRARIES})
    endif()
  endif()
  message(STATUS "FlashDEG: using vendored SciPy L-BFGS-B")
endif()

add_library(flashdeg_core
  src/core/csv.cpp
  src/core/design.cpp
  src/core/executor.cpp
  src/core/file_io.cpp
  src/core/linalg.cpp
  src/core/nb.cpp
  src/core/numeric_backend.cpp
  src/core/numpy_compat.cpp
  src/core/optimize.cpp
  src/core/normalization.cpp
  src/core/pipeline.cpp
  src/core/profile.cpp
  src/core/pydeseq2_dds.cpp
  src/core/pydeseq2_ds.cpp
  src/core/pydeseq2_grid_search.cpp
  src/core/pydeseq2_preprocessing.cpp
  src/core/pydeseq2_utils.cpp
  src/core/special.cpp
  src/core/table.cpp
)

target_include_directories(flashdeg_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(flashdeg_core PUBLIC cxx_std_20)

if(Eigen3_FOUND)
  target_link_libraries(flashdeg_core PRIVATE Eigen3::Eigen)
  target_compile_definitions(flashdeg_core PRIVATE FLASHDEG_HAVE_EIGEN=1)
endif()

if(Boost_FOUND)
  target_include_directories(flashdeg_core PRIVATE ${Boost_INCLUDE_DIRS})
  target_compile_definitions(flashdeg_core PRIVATE FLASHDEG_HAVE_BOOST_MATH=1)
endif()

if(OpenBLAS_FOUND)
  target_link_libraries(flashdeg_core PRIVATE OpenBLAS::OpenBLAS)
  target_compile_definitions(flashdeg_core
    PRIVATE
      FLASHDEG_HAVE_OPENBLAS=1
      FLASHDEG_HAVE_BLAS_LAPACK=1
      EIGEN_USE_BLAS=1)
elseif(BLAS_FOUND AND LAPACK_FOUND)
  target_link_libraries(flashdeg_core PRIVATE ${BLAS_LIBRARIES} ${LAPACK_LIBRARIES})
  target_compile_definitions(flashdeg_core
    PRIVATE
      FLASHDEG_HAVE_BLAS_LAPACK=1
      EIGEN_USE_BLAS=1)
endif()

if(TARGET scipy_lbfgsb)
  target_link_libraries(flashdeg_core PRIVATE scipy_lbfgsb)
  target_compile_definitions(flashdeg_core PRIVATE FLASHDEG_HAVE_SCIPY_LBFGSB=1)
endif()

if(MSVC)
  # Existing build trees may have cached a localized prefix before VSLANG was
  # forced. Keep Ninja's parser prefix aligned with the compile launcher.
  set(CMAKE_CL_SHOWINCLUDES_PREFIX "Note: including file: " CACHE INTERNAL
      "MSVC /showIncludes prefix for Ninja dependency scanning" FORCE)
  set_property(GLOBAL PROPERTY RULE_LAUNCH_COMPILE
    "${CMAKE_COMMAND} -E env VSLANG=1033")
  target_compile_options(flashdeg_core PRIVATE /W4 /permissive- /fp:precise)
  target_compile_definitions(flashdeg_core
    PRIVATE
      NOMINMAX
      WIN32_LEAN_AND_MEAN
      FLASHDEG_MSVC_SHOWINCLUDES_ENGLISH=1)
  target_link_libraries(flashdeg_core PRIVATE psapi)
else()
  target_compile_options(flashdeg_core PRIVATE -Wall -Wextra -Wpedantic -fno-fast-math)
endif()

add_executable(flashdeg
  src/cli/main.cpp
)
target_link_libraries(flashdeg PRIVATE flashdeg_core)
target_compile_features(flashdeg PRIVATE cxx_std_20)
target_compile_definitions(flashdeg
  PRIVATE
    FLASHDEG_DEFAULT_COMPAT_MODE="${FLASHDEG_DEFAULT_COMPAT_MODE}"
    FLASHDEG_GIT_REV="${FLASHDEG_GIT_REV}"
    FLASHDEG_BUILD_DATE="${FLASHDEG_BUILD_DATE}"
    FLASHDEG_BUILD_USE_EIGEN="$<IF:$<BOOL:${FLASHDEG_USE_EIGEN}>,ON,OFF>"
    FLASHDEG_BUILD_USE_BOOST_MATH="$<IF:$<BOOL:${FLASHDEG_USE_BOOST_MATH}>,ON,OFF>"
    FLASHDEG_BUILD_USE_BLAS="$<IF:$<BOOL:${FLASHDEG_USE_BLAS}>,ON,OFF>"
    FLASHDEG_BUILD_USE_SCIPY_LBFGSB="$<IF:$<BOOL:${FLASHDEG_USE_SCIPY_LBFGSB}>,ON,OFF>"
    FLASHDEG_ENABLE_DEV_OPTIONS=$<IF:$<BOOL:${FLASHDEG_ENABLE_DEV_OPTIONS}>,1,0>
    FLASHDEG_ALLOW_NO_DEPS_BUILD=$<IF:$<BOOL:${FLASHDEG_ALLOW_NO_DEPS_BUILD}>,1,0>
    FLASHDEG_BUILD_ENABLE_DEV_OPTIONS="$<IF:$<BOOL:${FLASHDEG_ENABLE_DEV_OPTIONS}>,ON,OFF>"
    FLASHDEG_BUILD_ALLOW_NO_DEPS="$<IF:$<BOOL:${FLASHDEG_ALLOW_NO_DEPS_BUILD}>,ON,OFF>")
if(MSVC)
  target_compile_definitions(flashdeg PRIVATE FLASHDEG_MSVC_SHOWINCLUDES_ENGLISH=1)
endif()

install(TARGETS flashdeg
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES LICENSE THIRD_PARTY_NOTICES.md
  DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
install(DIRECTORY LICENSES/
  DESTINATION ${CMAKE_INSTALL_DOCDIR}/LICENSES
)

if(FLASHDEG_BUILD_TESTS)
  enable_testing()
  find_package(Python3 COMPONENTS Interpreter QUIET)

  add_executable(flashdeg_tests
    tests/test_main.cpp
  )
  target_link_libraries(flashdeg_tests PRIVATE flashdeg_core)
  target_compile_features(flashdeg_tests PRIVATE cxx_std_20)
  target_compile_definitions(flashdeg_tests
    PRIVATE
      FLASHDEG_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
  )
  if(MSVC)
    target_compile_definitions(flashdeg_tests
      PRIVATE
        FLASHDEG_MSVC_SHOWINCLUDES_ENGLISH=1
    )
  endif()

  add_test(NAME flashdeg_tests COMMAND flashdeg_tests)
  add_test(NAME flashdeg_help COMMAND flashdeg --help)
  add_test(NAME flashdeg_version COMMAND flashdeg --version)
  add_test(NAME flashdeg_build_info COMMAND flashdeg --build-info)
  if(NOT FLASHDEG_ENABLE_DEV_OPTIONS)
    set(_flashdeg_release_rejected_dev_options
      "--compat-mode|pydeseq2"
      "--write-mu-hat|mu_hat.csv"
      "--write-cooks|cooks.csv"
      "--write-lfc-log2|lfc_log2.csv"
      "--write-vst-trend-coeffs|vst_trend_coeffs.csv"
    )
    foreach(_flashdeg_rejected_option_pair IN LISTS _flashdeg_release_rejected_dev_options)
      string(REPLACE "|" ";" _flashdeg_rejected_option_pair "${_flashdeg_rejected_option_pair}")
      list(GET _flashdeg_rejected_option_pair 0 _flashdeg_rejected_option)
      list(GET _flashdeg_rejected_option_pair 1 _flashdeg_rejected_path)
      string(REPLACE "--" "" _flashdeg_rejected_test_suffix "${_flashdeg_rejected_option}")
      string(REPLACE "-" "_" _flashdeg_rejected_test_suffix "${_flashdeg_rejected_test_suffix}")
      add_test(NAME flashdeg_release_rejects_${_flashdeg_rejected_test_suffix}
        COMMAND flashdeg run --counts missing-counts.csv --metadata missing-metadata.csv
                ${_flashdeg_rejected_option} ${_flashdeg_rejected_path} --dry-run)
      set_tests_properties(flashdeg_release_rejects_${_flashdeg_rejected_test_suffix}
        PROPERTIES
          WILL_FAIL TRUE)
    endforeach()
  endif()

  if(TARGET scipy_lbfgsb)
    add_executable(blas_fortran_symbol_test
      tests/blas_fortran_symbol_test.cpp
    )
    target_link_libraries(blas_fortran_symbol_test PRIVATE scipy_lbfgsb)
    add_test(NAME blas_fortran_symbol_test COMMAND blas_fortran_symbol_test)
  endif()

  if(Python3_Interpreter_FOUND)
    add_test(NAME oracle_interaction
      COMMAND ${Python3_EXECUTABLE}
              ${CMAKE_CURRENT_SOURCE_DIR}/tools/run_oracle_interaction.py
              --flashdeg $<TARGET_FILE:flashdeg>
              --fixtures ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/oracle_interaction
              --out-dir ${CMAKE_CURRENT_BINARY_DIR}/oracle_interaction)
  endif()
endif()
