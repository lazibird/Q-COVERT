string(TOLOWER "${RINGDORAM_SGX_MODE}" RINGDORAM_SETUP_SGX_MODE)
add_executable(qcovert_fixture_generator ../native/qcovert_fixture.cpp)
target_link_libraries(qcovert_fixture_generator PRIVATE RingDORAM::provider)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/qcovert_fixture.hpp"
    COMMAND qcovert_fixture_generator "${CMAKE_CURRENT_BINARY_DIR}/qcovert_fixture.hpp"
    DEPENDS qcovert_fixture_generator VERBATIM)
add_custom_target(qcovert_fixture_header DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/qcovert_fixture.hpp")

set(RINGDORAM_SGX_HEAP_SIZE "128M" CACHE STRING "Enclave heap for smoke/benchmark")
set(RINGDORAM_SGX_DATASET "" CACHE STRING "Dataset whose SGX SIM heap is read from the profile JSON")
set(RINGDORAM_SGX_PROFILE_JSON "${QCOVERT_PS_SOURCE_DIR}/config/dataset_defaults.json" CACHE FILEPATH
    "Dataset profile JSON for SGX SIM heap")
if(RINGDORAM_SGX_DATASET)
    if(NOT RINGDORAM_SGX_MODE STREQUAL "SIM")
        message(FATAL_ERROR "RINGDORAM_SGX_DATASET currently configures SGX SIM only")
    endif()
    if(NOT EXISTS "${RINGDORAM_SGX_PROFILE_JSON}")
        message(FATAL_ERROR "SGX dataset profile does not exist: ${RINGDORAM_SGX_PROFILE_JSON}")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${RINGDORAM_SGX_PROFILE_JSON}")
    file(READ "${RINGDORAM_SGX_PROFILE_JSON}" QCOVERT_PROFILE_JSON)
    string(JSON QCOVERT_SGX_HEAP_BYTES ERROR_VARIABLE QCOVERT_SGX_HEAP_ERROR GET
        "${QCOVERT_PROFILE_JSON}" datasets "${RINGDORAM_SGX_DATASET}" execution sgx_heap_bytes)
    if(QCOVERT_SGX_HEAP_ERROR)
        message(FATAL_ERROR "Cannot read execution.sgx_heap_bytes for ${RINGDORAM_SGX_DATASET}: ${QCOVERT_SGX_HEAP_ERROR}")
    endif()
    if(NOT "${QCOVERT_SGX_HEAP_BYTES}" MATCHES "^[0-9]+$")
        message(FATAL_ERROR "execution.sgx_heap_bytes must be an integer byte count")
    endif()
    math(EXPR QCOVERT_SGX_HEAP_MIB "${QCOVERT_SGX_HEAP_BYTES} / 1048576")
    math(EXPR QCOVERT_SGX_HEAP_ROUNDTRIP "${QCOVERT_SGX_HEAP_MIB} * 1048576")
    if(QCOVERT_SGX_HEAP_MIB LESS 128 OR NOT QCOVERT_SGX_HEAP_ROUNDTRIP EQUAL QCOVERT_SGX_HEAP_BYTES)
        message(FATAL_ERROR "execution.sgx_heap_bytes must be a whole MiB and at least 128 MiB")
    endif()
    set(RINGDORAM_SGX_HEAP_SIZE "${QCOVERT_SGX_HEAP_MIB}M" CACHE STRING
        "Enclave heap for smoke/benchmark" FORCE)
    message(STATUS "SGX SIM heap from ${RINGDORAM_SGX_DATASET}: ${RINGDORAM_SGX_HEAP_SIZE}")
endif()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/enclave.config.toml" QCOVERT_ENCLAVE_CONFIG_TEMPLATE)
string(REPLACE "128M" "${RINGDORAM_SGX_HEAP_SIZE}" QCOVERT_ENCLAVE_CONFIG "${QCOVERT_ENCLAVE_CONFIG_TEMPLATE}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/benchmark.enclave.toml" "${QCOVERT_ENCLAVE_CONFIG}")

set(RINGDORAM_SGX_EDL "${CMAKE_CURRENT_SOURCE_DIR}/ringdoram_sgx.edl")

sgx_sdk_get_setting(SDK_DIR RINGDORAM_SGX_SDK_DIR)
sgx_sdk_get_setting(LIB_DIR RINGDORAM_SGX_LIB_DIR)

sgx_sdk_generate_edl(
    NAME ringdoram_sgx
    EDL "${RINGDORAM_SGX_EDL}"
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated"
    OUT_TRUSTED_C RINGDORAM_SGX_TRUSTED_C
    OUT_TRUSTED_H RINGDORAM_SGX_TRUSTED_H
    OUT_UNTRUSTED_C RINGDORAM_SGX_UNTRUSTED_C
    OUT_UNTRUSTED_H RINGDORAM_SGX_UNTRUSTED_H
    OUT_DIR_VAR RINGDORAM_SGX_GENERATED_DIR
    OUT_TARGET RINGDORAM_SGX_EDL_TARGET
)

set(RINGDORAM_SGX_ENCLAVE_SOURCES
    enclave.cpp
    qcovert_benchmark.cpp
    setup_benchmark.cpp
    evict_benchmark.cpp
    "${QCOVERT_PS_SOURCE_DIR}/src/qcovert/crypto.cpp"
    cxx_compat.cpp
    "${RINGDORAM_SGX_TRUSTED_C}"
    "${QCOVERT_PS_SOURCE_DIR}/src/sonic/crypto/impl/sgx/random.cpp"
    "${QCOVERT_PS_SOURCE_DIR}/src/sonic/crypto/impl/sgx/prng.cpp"
    "${QCOVERT_PS_SOURCE_DIR}/src/sonic/sgxbridge_enclave_src/runtime.cpp"
    "${QCOVERT_PS_SOURCE_DIR}/src/sonic/sgxbridge_enclave_src/threadpool_portal.cpp"
    "${QCOVERT_PS_SOURCE_DIR}/src/sonic/sgxbridge_enclave_src/threadpool_provider.cpp"
    "${QCOVERT_PS_SOURCE_DIR}/src/sonic/sgxbridge_enclave_src/threadpool_registry.cpp"
    "${QCOVERT_PS_SOURCE_DIR}/src/sonic/sgxbridge_enclave_src/threadpool_worker.cpp")

sgx_sdk_add_enclave(ringdoram_sgx_enclave
    SOURCES
        ${RINGDORAM_SGX_ENCLAVE_SOURCES}
    HEADERS "${RINGDORAM_SGX_TRUSTED_H}"
    INCLUDE_DIRS
        "${RINGDORAM_SGX_GENERATED_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${QCOVERT_PS_SOURCE_DIR}/include"
        "${QCOVERT_PS_SOURCE_DIR}/src"
    EDL_TARGET "${RINGDORAM_SGX_EDL_TARGET}"
    CONFIG_TOML "${CMAKE_CURRENT_BINARY_DIR}/benchmark.enclave.toml"
    VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/enclave.lds"
    OUTPUT_NAME ringdoram_enclave
    SIGNED_OUTPUT_VAR RINGDORAM_SGX_SIGNED_ENCLAVE
    COMPILE_DEFINITIONS
        SONIC_ORAM_METRICS=1
        RINGDORAM_SETUP_SGX_MODE="${RINGDORAM_SETUP_SGX_MODE}"
        SN_SGX_ENCLAVE=1
        SONIC_NO_OS=1
        SONIC_THREADS_HAS_OS=0
        SONIC_THREADS_HAS_CHRONO=0
        RINGDORAM_CRYPTO_BACKEND_SGX=1
    COMPILE_OPTIONS -masm=intel
    LINK_LIBS "${RINGDORAM_SGX_LIB_DIR}/libsgx_pthread.a"
)

sgx_sdk_add_host(ringdoram_sgx_host
    SOURCES
        host.cpp
        "${QCOVERT_PS_SOURCE_DIR}/src/qcovert/storage_ocalls.cpp"
        "${RINGDORAM_SGX_UNTRUSTED_C}"
        "${QCOVERT_PS_SOURCE_DIR}/src/sonic/sgxbridge_host_src/runtime_support.cpp"
        "${QCOVERT_PS_SOURCE_DIR}/src/sonic/sgxbridge_host_src/threadpool_manager.cpp"
        "${QCOVERT_PS_SOURCE_DIR}/src/sonic/sgxbridge_host_src/threadpool_ocalls.cpp"
    HEADERS "${RINGDORAM_SGX_UNTRUSTED_H}"
    INCLUDE_DIRS
        "${RINGDORAM_SGX_GENERATED_DIR}"
        "${CMAKE_CURRENT_BINARY_DIR}"
        "${QCOVERT_PS_SOURCE_DIR}/include"
        "${QCOVERT_PS_SOURCE_DIR}/src"
    LINK_LIBS ringdoram_threads
    EDL_TARGET "${RINGDORAM_SGX_EDL_TARGET}"
    SIGNED_ENCLAVE "${RINGDORAM_SGX_SIGNED_ENCLAVE}"
    COPY_SIGNED
)

sgx_sdk_get_sign_target(ringdoram_sgx_enclave RINGDORAM_SGX_SIGN_TARGET)
add_dependencies(ringdoram_sgx_host ringdoram_sgx_enclave "${RINGDORAM_SGX_SIGN_TARGET}")
# The SDK's POST_BUILD copy runs only when the host relinks. Refresh the
# companion image also when an enclave-only edit triggers a new signature.
add_custom_target(ringdoram_sgx_refresh_image
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${RINGDORAM_SGX_SIGNED_ENCLAVE}"
        "${CMAKE_CURRENT_BINARY_DIR}/ringdoram_enclave.signed.so"
    DEPENDS "${RINGDORAM_SGX_SIGN_TARGET}"
    VERBATIM)
add_dependencies(ringdoram_sgx_host ringdoram_sgx_refresh_image)

# Extra heap variants: the enclave is compiled once, then signed again with a
# different HeapMaxSize per entry, so one build tree can serve datasets with
# different heap budgets. benchmark.py picks the smallest image whose heap
# covers the dataset profile's execution.sgx_heap_bytes.
set(RINGDORAM_SGX_HEAP_VARIANTS "" CACHE STRING
    "Additional enclave heap sizes (e.g. 196608M or 192G), one signed image per entry")
if(RINGDORAM_SGX_HEAP_SIZE MATCHES "^([1-9][0-9]*)[Mm]$")
    set(RINGDORAM_SGX_HEAP_DEFAULT_MIB "${CMAKE_MATCH_1}")
elseif(RINGDORAM_SGX_HEAP_SIZE MATCHES "^([1-9][0-9]*)[Gg]$")
    math(EXPR RINGDORAM_SGX_HEAP_DEFAULT_MIB "${CMAKE_MATCH_1} * 1024")
else()
    message(FATAL_ERROR "RINGDORAM_SGX_HEAP_SIZE must be <N>M or <N>G, got '${RINGDORAM_SGX_HEAP_SIZE}'")
endif()
sgx_sdk_get_setting(SIGN RINGDORAM_SGX_SIGN_TOOL)
get_target_property(RINGDORAM_SGX_SIGNING_KEY_PATH ringdoram_sgx_enclave SGXSDK_SIGNING_KEY)
foreach(variant_heap IN LISTS RINGDORAM_SGX_HEAP_VARIANTS)
    if(variant_heap MATCHES "^([1-9][0-9]*)[Mm]$")
        set(variant_mib "${CMAKE_MATCH_1}")
    elseif(variant_heap MATCHES "^([1-9][0-9]*)[Gg]$")
        math(EXPR variant_mib "${CMAKE_MATCH_1} * 1024")
    else()
        message(FATAL_ERROR "RINGDORAM_SGX_HEAP_VARIANTS entries must be <N>M or <N>G, got '${variant_heap}'")
    endif()
    if(variant_mib LESS 128)
        message(FATAL_ERROR "RINGDORAM_SGX_HEAP_VARIANTS entries must be at least 128 MiB, got '${variant_heap}'")
    endif()
    if(variant_mib EQUAL RINGDORAM_SGX_HEAP_DEFAULT_MIB)
        continue() # same heap as the default ringdoram_enclave.signed.so
    endif()
    string(REPLACE "128M" "${variant_mib}M" variant_config "${QCOVERT_ENCLAVE_CONFIG_TEMPLATE}")
    set(variant_toml "${CMAKE_CURRENT_BINARY_DIR}/benchmark_h${variant_mib}m.enclave.toml")
    file(WRITE "${variant_toml}" "${variant_config}")
    sgx_sdk_generate_enclave_config(
        NAME "ringdoram_sgx_h${variant_mib}m"
        INPUT "${variant_toml}"
        OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated"
        OUT_CONFIG variant_xml
        OUT_TARGET variant_xml_target
    )
    set(variant_signed "${CMAKE_CURRENT_BINARY_DIR}/ringdoram_enclave_h${variant_mib}m.signed.so")
    add_custom_command(
        OUTPUT "${variant_signed}"
        COMMAND "${RINGDORAM_SGX_SIGN_TOOL}" sign
            -key "${RINGDORAM_SGX_SIGNING_KEY_PATH}"
            -enclave "$<TARGET_FILE:ringdoram_sgx_enclave>"
            -out "${variant_signed}"
            -config "${variant_xml}"
        DEPENDS ringdoram_sgx_enclave "${variant_xml}" "${RINGDORAM_SGX_SIGNING_KEY_PATH}"
        COMMENT "signing enclave (${variant_mib} MiB heap) -> ${variant_signed}"
        VERBATIM)
    add_custom_target("ringdoram_sgx_enclave_h${variant_mib}m_sign" ALL DEPENDS "${variant_signed}")
    add_dependencies("ringdoram_sgx_enclave_h${variant_mib}m_sign" ringdoram_sgx_enclave "${variant_xml_target}")
    add_dependencies(ringdoram_sgx_host "ringdoram_sgx_enclave_h${variant_mib}m_sign")
endforeach()

if(RINGDORAM_BUILD_TESTS)
    add_test(
        NAME ringdoram.sgx_sim
        COMMAND $<TARGET_FILE:ringdoram_sgx_host> "${RINGDORAM_SGX_SIGNED_ENCLAVE}"
    )
    set_tests_properties(ringdoram.sgx_sim PROPERTIES TIMEOUT 120)
    if(RINGDORAM_SGX_MODE STREQUAL "SIM")
        # Minimal argv (argc=4), R=0 and stdout CSV used to take the wrong CLI path.
        add_test(NAME ringdoram.sgx_setup COMMAND $<TARGET_FILE:ringdoram_sgx_host>
            "${RINGDORAM_SGX_SIGNED_ENCLAVE}" --setup smoke,256,64,0)
        add_test(NAME ringdoram.sgx_setup_parallel COMMAND $<TARGET_FILE:ringdoram_sgx_host>
            "${RINGDORAM_SGX_SIGNED_ENCLAVE}" --setup --z 8 --s 8 --a 8
            --threads 4 --evict-batch 4 parallel,4099,1280,2)
        set_tests_properties(ringdoram.sgx_setup ringdoram.sgx_setup_parallel PROPERTIES
            TIMEOUT 120 PASS_REGULAR_EXPRESSION "sgx-sim,sequential")
    endif()

endif()

add_dependencies(ringdoram_sgx_enclave qcovert_fixture_header)

target_compile_definitions(ringdoram_sgx_host PRIVATE QCOVERT_SGX_MODE="${RINGDORAM_SGX_MODE}")

if(RINGDORAM_BUILD_COMPASS)
    target_sources(ringdoram_sgx_enclave PRIVATE "${QCOVERT_PS_SOURCE_DIR}/compass/sgx.cpp")
    target_include_directories(ringdoram_sgx_enclave PRIVATE "${QCOVERT_PS_SOURCE_DIR}/compass/include" "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(ringdoram_sgx_enclave PRIVATE ${COMPASS_DEFINITIONS} RINGDORAM_SGX_HAS_COMPASS=1)
    sgx_sdk_add_host(compass_sgx_host
        SOURCES host.cpp
            "${QCOVERT_PS_SOURCE_DIR}/src/qcovert/storage_ocalls.cpp"
            "${RINGDORAM_SGX_UNTRUSTED_C}"
            "${QCOVERT_PS_SOURCE_DIR}/src/sonic/sgxbridge_host_src/runtime_support.cpp"
            "${QCOVERT_PS_SOURCE_DIR}/src/sonic/sgxbridge_host_src/threadpool_manager.cpp"
            "${QCOVERT_PS_SOURCE_DIR}/src/sonic/sgxbridge_host_src/threadpool_ocalls.cpp"
        HEADERS "${RINGDORAM_SGX_UNTRUSTED_H}"
        INCLUDE_DIRS "${RINGDORAM_SGX_GENERATED_DIR}" "${QCOVERT_PS_SOURCE_DIR}/include" "${QCOVERT_PS_SOURCE_DIR}/src"
        LINK_LIBS ringdoram_threads
        EDL_TARGET "${RINGDORAM_SGX_EDL_TARGET}"
        SIGNED_ENCLAVE "${RINGDORAM_SGX_SIGNED_ENCLAVE}")
    target_compile_definitions(compass_sgx_host PRIVATE ${COMPASS_DEFINITIONS}
        COMPASS_SGX_HOST_MODE=1 QCOVERT_SGX_MODE="${RINGDORAM_SGX_MODE}")
    target_include_directories(compass_sgx_host PRIVATE "${QCOVERT_PS_SOURCE_DIR}/compass/include")
    set_target_properties(compass_sgx_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${QCOVERT_PS_BINARY_DIR}/compass")
    add_dependencies(compass_sgx_host ringdoram_sgx_refresh_image)
    if(RINGDORAM_BUILD_TESTS AND RINGDORAM_BENCHMARK_PYTHON AND RINGDORAM_BUILD_EXAMPLES)
        string(TOLOWER "sgx-${RINGDORAM_SGX_MODE}" COMPASS_TEST_MODE)
        add_test(NAME compass.sgx_pipeline COMMAND "${RINGDORAM_BENCHMARK_PYTHON}"
            "${QCOVERT_PS_SOURCE_DIR}/compass/pipeline_test.py" "${QCOVERT_PS_BINARY_DIR}" "${COMPASS_TEST_MODE}")
        set_tests_properties(compass.sgx_pipeline PROPERTIES
            ENVIRONMENT "OMP_NUM_THREADS=1;OPENBLAS_NUM_THREADS=1" TIMEOUT 240)
    endif()
endif()

if(RINGDORAM_BUILD_BISON)
    target_sources(ringdoram_sgx_enclave PRIVATE "${QCOVERT_PS_SOURCE_DIR}/bison/sgx.cpp")
    target_include_directories(ringdoram_sgx_enclave PRIVATE "${QCOVERT_PS_SOURCE_DIR}/bison/include" "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(ringdoram_sgx_enclave PRIVATE ${BISON_DEFINITIONS} RINGDORAM_SGX_HAS_BISON=1)
    set_source_files_properties("${QCOVERT_PS_SOURCE_DIR}/bison/sgx.cpp" PROPERTIES COMPILE_OPTIONS "-ffp-contract=off")
    sgx_sdk_add_host(bison_sgx_host
        SOURCES host.cpp
            "${QCOVERT_PS_SOURCE_DIR}/src/qcovert/storage_ocalls.cpp"
            "${RINGDORAM_SGX_UNTRUSTED_C}"
            "${QCOVERT_PS_SOURCE_DIR}/src/sonic/sgxbridge_host_src/runtime_support.cpp"
            "${QCOVERT_PS_SOURCE_DIR}/src/sonic/sgxbridge_host_src/threadpool_manager.cpp"
            "${QCOVERT_PS_SOURCE_DIR}/src/sonic/sgxbridge_host_src/threadpool_ocalls.cpp"
        HEADERS "${RINGDORAM_SGX_UNTRUSTED_H}"
        INCLUDE_DIRS "${RINGDORAM_SGX_GENERATED_DIR}" "${QCOVERT_PS_SOURCE_DIR}/include" "${QCOVERT_PS_SOURCE_DIR}/src"
        LINK_LIBS ringdoram_threads
        EDL_TARGET "${RINGDORAM_SGX_EDL_TARGET}"
        SIGNED_ENCLAVE "${RINGDORAM_SGX_SIGNED_ENCLAVE}")
    target_compile_definitions(bison_sgx_host PRIVATE ${BISON_DEFINITIONS}
        BISON_SGX_HOST_MODE=1 QCOVERT_SGX_MODE="${RINGDORAM_SGX_MODE}")
    target_include_directories(bison_sgx_host PRIVATE "${QCOVERT_PS_SOURCE_DIR}/bison/include")
    set_target_properties(bison_sgx_host PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${QCOVERT_PS_BINARY_DIR}/bison")
    add_dependencies(bison_sgx_host ringdoram_sgx_refresh_image)
    # Extra layouts: one enclave+host pair per RINGDORAM_BISON_LAYOUTS entry,
    # all emitted into the same build tree (bison_sgx_host_d..._... plus
    # bison_enclave_d..._....signed.so under ${QCOVERT_PS_BINARY_DIR}/bison).
    foreach(layout IN LISTS RINGDORAM_BISON_LAYOUTS)
        string(REPLACE ":" ";" parts "${layout}")
        list(LENGTH parts parts_count)
        if(NOT parts_count EQUAL 4)
            message(FATAL_ERROR "RINGDORAM_BISON_LAYOUTS entries must be dim:degree:pq_bytes:scalar, got '${layout}'")
        endif()
        list(GET parts 0 layout_dim)
        list(GET parts 1 layout_degree)
        list(GET parts 2 layout_pq)
        list(GET parts 3 layout_scalar)
        if(layout_dim STREQUAL BISON_DIM AND layout_degree STREQUAL BISON_DEGREE
            AND layout_pq STREQUAL BISON_PQ_BYTES AND layout_scalar STREQUAL BISON_SCALAR)
            continue() # same as the default bison_sgx_host
        endif()
        set(layout_suffix "_d${layout_dim}_r${layout_degree}_p${layout_pq}_${layout_scalar}")
        bison_layout_definitions(${layout_dim} ${layout_degree} ${layout_pq} ${layout_scalar} layout_definitions)
        set(layout_enclave_sources ${RINGDORAM_SGX_ENCLAVE_SOURCES} "${QCOVERT_PS_SOURCE_DIR}/bison/sgx.cpp")
        set(layout_enclave_includes "${QCOVERT_PS_SOURCE_DIR}/bison/include" "${CMAKE_CURRENT_SOURCE_DIR}")
        set(layout_enclave_definitions ${layout_definitions} RINGDORAM_SGX_HAS_BISON=1)
        if(RINGDORAM_BUILD_COMPASS)
            list(APPEND layout_enclave_sources "${QCOVERT_PS_SOURCE_DIR}/compass/sgx.cpp")
            list(APPEND layout_enclave_includes "${QCOVERT_PS_SOURCE_DIR}/compass/include")
            list(APPEND layout_enclave_definitions ${COMPASS_DEFINITIONS} RINGDORAM_SGX_HAS_COMPASS=1)
        endif()
        sgx_sdk_add_enclave(bison_sgx_enclave${layout_suffix}
            SOURCES ${layout_enclave_sources}
            HEADERS "${RINGDORAM_SGX_TRUSTED_H}"
            INCLUDE_DIRS
                "${RINGDORAM_SGX_GENERATED_DIR}"
                "${CMAKE_CURRENT_BINARY_DIR}"
                "${QCOVERT_PS_SOURCE_DIR}/include"
                "${QCOVERT_PS_SOURCE_DIR}/src"
                ${layout_enclave_includes}
            EDL_TARGET "${RINGDORAM_SGX_EDL_TARGET}"
            CONFIG_TOML "${CMAKE_CURRENT_BINARY_DIR}/benchmark.enclave.toml"
            VERSION_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/enclave.lds"
            OUTPUT_NAME bison_enclave${layout_suffix}
            SIGNED_OUTPUT_VAR layout_signed
            COMPILE_DEFINITIONS
                SONIC_ORAM_METRICS=1
                RINGDORAM_SETUP_SGX_MODE="${RINGDORAM_SETUP_SGX_MODE}"
                SN_SGX_ENCLAVE=1
                SONIC_NO_OS=1
                SONIC_THREADS_HAS_OS=0
                SONIC_THREADS_HAS_CHRONO=0
                RINGDORAM_CRYPTO_BACKEND_SGX=1
                ${layout_enclave_definitions}
            COMPILE_OPTIONS -masm=intel
            LINK_LIBS "${RINGDORAM_SGX_LIB_DIR}/libsgx_pthread.a"
        )
        add_dependencies(bison_sgx_enclave${layout_suffix} qcovert_fixture_header)
        sgx_sdk_get_sign_target(bison_sgx_enclave${layout_suffix} layout_sign_target)
        add_custom_target(bison_sgx_refresh${layout_suffix}
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${layout_signed}"
                "${QCOVERT_PS_BINARY_DIR}/bison/bison_enclave${layout_suffix}.signed.so"
            DEPENDS "${layout_sign_target}"
            VERBATIM)
        sgx_sdk_add_host(bison_sgx_host${layout_suffix}
            SOURCES host.cpp
                "${QCOVERT_PS_SOURCE_DIR}/src/qcovert/storage_ocalls.cpp"
                "${RINGDORAM_SGX_UNTRUSTED_C}"
                "${QCOVERT_PS_SOURCE_DIR}/src/sonic/sgxbridge_host_src/runtime_support.cpp"
                "${QCOVERT_PS_SOURCE_DIR}/src/sonic/sgxbridge_host_src/threadpool_manager.cpp"
                "${QCOVERT_PS_SOURCE_DIR}/src/sonic/sgxbridge_host_src/threadpool_ocalls.cpp"
            HEADERS "${RINGDORAM_SGX_UNTRUSTED_H}"
            INCLUDE_DIRS "${RINGDORAM_SGX_GENERATED_DIR}" "${QCOVERT_PS_SOURCE_DIR}/include" "${QCOVERT_PS_SOURCE_DIR}/src"
            LINK_LIBS ringdoram_threads
            EDL_TARGET "${RINGDORAM_SGX_EDL_TARGET}"
            SIGNED_ENCLAVE "${layout_signed}")
        target_compile_definitions(bison_sgx_host${layout_suffix} PRIVATE ${layout_definitions}
            BISON_SGX_HOST_MODE=1 QCOVERT_SGX_MODE="${RINGDORAM_SGX_MODE}")
        target_include_directories(bison_sgx_host${layout_suffix} PRIVATE "${QCOVERT_PS_SOURCE_DIR}/bison/include")
        set_target_properties(bison_sgx_host${layout_suffix} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${QCOVERT_PS_BINARY_DIR}/bison")
        add_dependencies(bison_sgx_host${layout_suffix} bison_sgx_refresh${layout_suffix})
    endforeach()
    if(RINGDORAM_BUILD_TESTS AND RINGDORAM_BENCHMARK_PYTHON AND RINGDORAM_BUILD_EXAMPLES)
        string(TOLOWER "sgx-${RINGDORAM_SGX_MODE}" BISON_TEST_MODE)
        add_test(NAME bison.sgx_pipeline COMMAND "${RINGDORAM_BENCHMARK_PYTHON}"
            "${QCOVERT_PS_SOURCE_DIR}/bison/pipeline_test.py" "${QCOVERT_PS_BINARY_DIR}" "${BISON_TEST_MODE}")
        set_tests_properties(bison.sgx_pipeline PROPERTIES
            ENVIRONMENT "OMP_NUM_THREADS=1;OPENBLAS_NUM_THREADS=1" TIMEOUT 240)
    endif()
endif()
