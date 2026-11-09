cmake_minimum_required(VERSION 3.20)

project(QCOVERT_PS VERSION 0.1.0 LANGUAGES C CXX)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/LocalDependencies.cmake")
include(CTest)
set(QCOVERT_PS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(QCOVERT_PS_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}")

option(RINGDORAM_BUILD_TESTS "Build Q-COVERT-PS tests" ${BUILD_TESTING})
option(RINGDORAM_BUILD_EXAMPLES "Build RingDORAM examples" ON)
option(RINGDORAM_ENABLE_ORAM_DEBUG "Enable expensive ORAM invariant checks" OFF)
option(RINGDORAM_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(RINGDORAM_ENABLE_SGX "Build Intel SGX host/enclave targets" OFF)
set(RINGDORAM_SGX_MODE "SIM" CACHE STRING "Intel SGX execution mode (SIM or HW)")
set_property(CACHE RINGDORAM_SGX_MODE PROPERTY STRINGS SIM HW)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(ringdoram_options INTERFACE)
target_compile_features(ringdoram_options INTERFACE cxx_std_20)
target_compile_options(ringdoram_options INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic;-Wno-unused-parameter;-Wno-unused-variable;-Wno-unused-function>
)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|X86_64|amd64|AMD64)$")
    target_compile_options(ringdoram_options INTERFACE
        $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-masm=intel>
    )
endif()
if(RINGDORAM_ENABLE_ORAM_DEBUG)
    target_compile_definitions(ringdoram_options INTERFACE ORAM_DEBUG=1 RINGDORAM_ENABLE_CONTRACT_CHECKS=1)
endif()
if(RINGDORAM_ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Sanitizers require GCC or Clang")
    endif()
    target_compile_options(ringdoram_options INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(ringdoram_options INTERFACE -fsanitize=address,undefined)
endif()

add_library(ringdoram_headers INTERFACE)
target_include_directories(ringdoram_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(ringdoram_headers INTERFACE ringdoram_options)

add_library(ringdoram_threads INTERFACE)
target_link_libraries(ringdoram_threads INTERFACE ringdoram_headers Threads::Threads)
target_compile_definitions(ringdoram_threads INTERFACE SONIC_THREADS_HAS_OS=1 SONIC_THREADS_HAS_CHRONO=1)

find_package(OpenSSL 3.0 REQUIRED)
add_library(ringdoram_crypto STATIC
    src/sonic/crypto/impl/openssl/prng.cpp
    src/sonic/crypto/impl/openssl/random.cpp
    src/sonic/crypto/impl/openssl/detail/openssl_utils.cpp
)
target_link_libraries(ringdoram_crypto PUBLIC ringdoram_headers OpenSSL::Crypto)
target_compile_definitions(ringdoram_crypto PUBLIC
    SONIC_CRYPTO_BACKEND_OPENSSL=1
    SONIC_OPENSSL_USE_LOWLEVEL=0
)

add_library(ringdoram_primitives INTERFACE)
target_link_libraries(ringdoram_primitives INTERFACE ringdoram_headers ringdoram_threads ringdoram_crypto)

add_library(ringdoram_core INTERFACE)
target_link_libraries(ringdoram_core INTERFACE ringdoram_primitives)

add_library(RingDORAM::primitives ALIAS ringdoram_primitives)
add_library(RingDORAM::core ALIAS ringdoram_core)

add_library(oblivious_topk INTERFACE)
target_link_libraries(oblivious_topk INTERFACE ringdoram_headers Threads::Threads)
add_library(oblivious_topk::oblivious_topk ALIAS oblivious_topk)

add_library(qcovert STATIC src/qcovert/crypto.cpp)
target_link_libraries(qcovert PUBLIC RingDORAM::core)
set_target_properties(qcovert PROPERTIES EXPORT_NAME qcovert)
add_library(RingDORAM::qcovert ALIAS qcovert)
add_library(qcovert_provider INTERFACE)
target_link_libraries(qcovert_provider INTERFACE qcovert)
target_include_directories(qcovert_provider SYSTEM INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/eigen>
    $<INSTALL_INTERFACE:include/qcovert/vendor/eigen>)
set_target_properties(qcovert_provider PROPERTIES EXPORT_NAME provider)
add_library(RingDORAM::provider ALIAS qcovert_provider)

set_target_properties(ringdoram_options PROPERTIES EXPORT_NAME options)
set_target_properties(ringdoram_headers PROPERTIES EXPORT_NAME headers)
set_target_properties(ringdoram_threads PROPERTIES EXPORT_NAME threads)
set_target_properties(ringdoram_crypto PROPERTIES EXPORT_NAME crypto)
set_target_properties(ringdoram_primitives PROPERTIES EXPORT_NAME primitives)
set_target_properties(ringdoram_core PROPERTIES EXPORT_NAME core)

if(RINGDORAM_BUILD_EXAMPLES)
    add_executable(qcovert_centroid_benchmark apps/native/centroid_benchmark.cpp)
    target_link_libraries(qcovert_centroid_benchmark PRIVATE RingDORAM::qcovert)
    add_executable(qcovert_benchmark apps/native/qcovert_benchmark.cpp)
    target_link_libraries(qcovert_benchmark PRIVATE RingDORAM::qcovert)
    target_compile_definitions(qcovert_benchmark PRIVATE SONIC_ORAM_METRICS=1)
    add_executable(qcovert_example apps/native/qcovert_example.cpp)
    target_link_libraries(qcovert_example PRIVATE RingDORAM::provider)
    add_executable(qcovert_prepare apps/native/qcovert_prepare.cpp)
    target_link_libraries(qcovert_prepare PRIVATE RingDORAM::provider)
    add_executable(oblivious_topk_example apps/native/topk_example.cpp)
    target_link_libraries(oblivious_topk_example PRIVATE RingDORAM::core)
    add_executable(oblivious_topk_benchmark apps/native/topk_benchmark.cpp)
    target_link_libraries(oblivious_topk_benchmark PRIVATE RingDORAM::core)
    add_executable(ringdoram_batch_example apps/native/batch_example.cpp)
    target_link_libraries(ringdoram_batch_example PRIVATE RingDORAM::core)

    add_executable(ringdoram_primitives_example apps/native/primitives_example.cpp)
    target_link_libraries(ringdoram_primitives_example PRIVATE RingDORAM::primitives)

    add_executable(ringdoram_setup_benchmark apps/native/setup_benchmark.cpp)
    target_link_libraries(ringdoram_setup_benchmark PRIVATE RingDORAM::core)
    target_compile_definitions(ringdoram_setup_benchmark PRIVATE SONIC_ORAM_METRICS=1)

    add_executable(ringdoram_evict_benchmark apps/native/evict_benchmark.cpp)
    target_link_libraries(ringdoram_evict_benchmark PRIVATE RingDORAM::core)

    add_executable(ringdoram_evict_count apps/native/evict_benchmark.cpp)
    target_link_libraries(ringdoram_evict_count PRIVATE RingDORAM::core)
    target_compile_definitions(ringdoram_evict_count PRIVATE RINGDORAM_COUNT_SORT_COMPARATORS=1)

    add_executable(ringdoram_table5_count apps/native/table5_count.cpp)
    target_link_libraries(ringdoram_table5_count PRIVATE RingDORAM::core)
    target_compile_definitions(ringdoram_table5_count PRIVATE RINGDORAM_COUNT_SORT_COMPARATORS=1)
endif()

if(RINGDORAM_BUILD_TESTS)
    include(CTest)
    add_executable(qcovert_stash_metrics_test tests/stash_metrics_test.cpp)
    target_link_libraries(qcovert_stash_metrics_test PRIVATE RingDORAM::core)
    target_compile_definitions(qcovert_stash_metrics_test PRIVATE SONIC_ORAM_METRICS=1)
    add_test(NAME qcovert.stash_metrics COMMAND qcovert_stash_metrics_test)
    set(RINGDORAM_BENCHMARK_PYTHON "" CACHE FILEPATH "Python interpreter with NumPy/FAISS for benchmark integration tests")
    if(RINGDORAM_BENCHMARK_PYTHON AND RINGDORAM_BUILD_EXAMPLES)
        add_test(NAME qcovert.benchmark_csv COMMAND "${RINGDORAM_BENCHMARK_PYTHON}"
            "${QCOVERT_PS_SOURCE_DIR}/tests/benchmark_test.py" "${QCOVERT_PS_BINARY_DIR}")
        set_tests_properties(qcovert.benchmark_csv PROPERTIES ENVIRONMENT "OMP_NUM_THREADS=1;OPENBLAS_NUM_THREADS=1;ASAN_OPTIONS=detect_leaks=0" TIMEOUT 180)
    endif()
    add_executable(qcovert_test tests/qcovert_test.cpp)
    target_link_libraries(qcovert_test PRIVATE RingDORAM::provider)
    add_test(NAME qcovert.correctness COMMAND qcovert_test)
    add_executable(oblivious_topk_test tests/topk_test.cpp)
    target_link_libraries(oblivious_topk_test PRIVATE RingDORAM::core)
    add_test(NAME qcovert.generic_topk COMMAND oblivious_topk_test)
    add_executable(ringdoram_primitives_test tests/primitives_test.cpp)
    target_link_libraries(ringdoram_primitives_test PRIVATE RingDORAM::primitives)
    add_test(NAME ringdoram.primitives COMMAND ringdoram_primitives_test)

    add_executable(ringdoram_batch_test tests/batch_test.cpp)
    target_link_libraries(ringdoram_batch_test PRIVATE RingDORAM::core)
    add_test(NAME ringdoram.batch COMMAND ringdoram_batch_test)

    add_executable(ringdoram_default_epoch_test tests/default_epoch_test.cpp)
    target_link_libraries(ringdoram_default_epoch_test PRIVATE RingDORAM::core)
    add_test(NAME ringdoram.default_epoch COMMAND ringdoram_default_epoch_test)

    add_executable(ringdoram_randomized_test tests/randomized_test.cpp)
    target_link_libraries(ringdoram_randomized_test PRIVATE RingDORAM::core)
    add_test(NAME ringdoram.randomized COMMAND ringdoram_randomized_test)

    add_executable(ringdoram_setup_test tests/setup_test.cpp)
    target_link_libraries(ringdoram_setup_test PRIVATE RingDORAM::core)
    add_test(NAME ringdoram.setup COMMAND ringdoram_setup_test)

    add_executable(ringdoram_setup_baseline_test tests/setup_baseline_test.cpp)
    target_link_libraries(ringdoram_setup_baseline_test PRIVATE RingDORAM::core)
    target_compile_definitions(ringdoram_setup_baseline_test PRIVATE SONIC_ORAM_METRICS=1)
    add_test(NAME ringdoram.setup_baseline COMMAND ringdoram_setup_baseline_test)
    set_tests_properties(ringdoram.setup_baseline PROPERTIES TIMEOUT 120)

    add_executable(ringdoram_evict_test tests/evict_test.cpp)
    target_link_libraries(ringdoram_evict_test PRIVATE RingDORAM::core)
    add_test(NAME ringdoram.evict COMMAND ringdoram_evict_test)

    if(RINGDORAM_ENABLE_SANITIZERS)
        set_tests_properties(ringdoram.setup_baseline PROPERTIES TIMEOUT 300)
        # LeakSanitizer cannot attach under the managed/ptraced test runner.
        # AddressSanitizer and UndefinedBehaviorSanitizer remain fully enabled.
        set_tests_properties(
            ringdoram.primitives ringdoram.batch ringdoram.default_epoch ringdoram.randomized ringdoram.setup
            ringdoram.setup_baseline ringdoram.evict qcovert.correctness qcovert.generic_topk qcovert.stash_metrics
            PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0"
        )
    endif()
endif()

option(RINGDORAM_BUILD_COMPASS "Build the independent doubly oblivious Compass baseline" ON)
if(RINGDORAM_BUILD_COMPASS)
    add_subdirectory(compass)
endif()

option(RINGDORAM_BUILD_BISON "Build the independent doubly oblivious BiSON baseline" OFF)
if(RINGDORAM_BUILD_BISON)
    add_subdirectory(bison)
endif()

if(RINGDORAM_ENABLE_SGX)
    set(SGX_SDK_DIR "/opt/intel/sgxsdk" CACHE PATH "Intel SGX SDK root")
    if(NOT EXISTS "${SGX_SDK_DIR}/include/sgx.h")
        message(FATAL_ERROR "RINGDORAM_ENABLE_SGX=ON but SGX SDK was not found at '${SGX_SDK_DIR}'")
    endif()
    include(cmake/sgxsdk.cmake)
    sgx_sdk_init(
        SDK_DIR "${SGX_SDK_DIR}"
        MODE "${RINGDORAM_SGX_MODE}"
        DEBUG ON
    )
    add_subdirectory(src/sgxcxx)
    add_subdirectory(apps/sgx)
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
configure_package_config_file(
    cmake/RingDORAMConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/RingDORAMConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RingDORAM"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/RingDORAMConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY src/sonic/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/sonic FILES_MATCHING PATTERN "*.hpp")
install(TARGETS oblivious_topk qcovert qcovert_provider ringdoram_crypto ringdoram_options ringdoram_headers ringdoram_threads ringdoram_primitives ringdoram_core
    EXPORT RingDORAMTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(EXPORT RingDORAMTargets
    NAMESPACE RingDORAM::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/RingDORAM
)
install(
    FILES
        "${CMAKE_CURRENT_BINARY_DIR}/RingDORAMConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/RingDORAMConfigVersion.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RingDORAM"
)

install(DIRECTORY vendor/eigen/Eigen DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/qcovert/vendor/eigen)
install(FILES vendor/oblivious_topk.LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/RingDORAM/licenses)
