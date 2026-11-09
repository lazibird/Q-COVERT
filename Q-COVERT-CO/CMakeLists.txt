cmake_minimum_required(VERSION 3.16)
project(QCOVERT_CO LANGUAGES CXX)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/LocalDependencies.cmake")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
include(CTest)
find_package(OpenSSL REQUIRED)
add_compile_definitions(QCOVERT_CO_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

find_package(OpenMP REQUIRED)

set(DOUBLE_ORAM_COMMON_SRC
    Common/Block.cpp
    Common/Bucket.cpp
    Common/RandomForOram.cpp
    Common/csprng.cpp
    Common/evp.cpp
)

set(DOUBLE_ORAM_RING_SRC
    FuORAM/FuORAM.cpp
    FuORAM/RemoteRing.cpp
)

set(DOUBLE_ORAM_CLUSTER_SRC
    QuORAM/QuORAM.cpp
)

set(DOUBLE_ORAM_DOUBLE_SRC
    DoubleORAM/DoubleORAM.cpp
)

add_library(double_oram_library
    ${DOUBLE_ORAM_COMMON_SRC}
    ${DOUBLE_ORAM_RING_SRC}
    ${DOUBLE_ORAM_CLUSTER_SRC}
    ${DOUBLE_ORAM_DOUBLE_SRC}
)

target_include_directories(double_oram_library
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/Common
        ${CMAKE_CURRENT_SOURCE_DIR}/Common/duthomhas
        ${CMAKE_CURRENT_SOURCE_DIR}/FuORAM
        ${CMAKE_CURRENT_SOURCE_DIR}/QuORAM
        ${CMAKE_CURRENT_SOURCE_DIR}/DoubleORAM
        ${CMAKE_CURRENT_SOURCE_DIR}/ivf-rabitq
)

target_link_libraries(double_oram_library
    PRIVATE
        OpenMP::OpenMP_CXX
    PUBLIC
        OpenSSL::Crypto
)

add_executable(test_double_oram_workflow
    test_double_oram_workflow.cpp
)

target_link_libraries(test_double_oram_workflow
    PRIVATE
        double_oram_library
)

add_executable(test_ann_double_oram_workflow
    test_ann_double_oram_workflow.cpp
)

target_compile_options(test_ann_double_oram_workflow
    PRIVATE
        -mavx2
)

target_link_libraries(test_ann_double_oram_workflow
    PRIVATE
        double_oram_library
)

add_executable(test_ivf_rabitq_double_oram
    test_ivf_rabitq_double_oram.cpp
)

target_compile_options(test_ivf_rabitq_double_oram
    PRIVATE
        -mavx2
)

target_compile_definitions(test_ivf_rabitq_double_oram
    PRIVATE
        DIM=128
        BB=128
        B_QUERY=4
)

target_link_libraries(test_ivf_rabitq_double_oram
    PRIVATE
        double_oram_library
)

add_executable(build_ivf_rabitq_index
    ivf-rabitq/build_index.cpp
)

target_compile_options(build_ivf_rabitq_index
    PRIVATE
        -mavx2
)

target_compile_definitions(build_ivf_rabitq_index
    PRIVATE
        DIM=128
        BB=128
        B_QUERY=4
)

target_include_directories(build_ivf_rabitq_index
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ivf-rabitq
)

add_executable(build_ivf_rabitq_index_d96
    ivf-rabitq/build_index.cpp
)

target_compile_options(build_ivf_rabitq_index_d96
    PRIVATE
        -mavx2
)

target_compile_definitions(build_ivf_rabitq_index_d96
    PRIVATE
        DIM=96
        BB=128
        B_QUERY=4
)

target_include_directories(build_ivf_rabitq_index_d96
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ivf-rabitq
)

add_executable(build_ivf_rabitq_index_d768
    ivf-rabitq/build_index.cpp
)

target_compile_options(build_ivf_rabitq_index_d768
    PRIVATE
        -mavx2
)

target_compile_definitions(build_ivf_rabitq_index_d768
    PRIVATE
        DIM=768
        BB=768
        B_QUERY=4
)

target_include_directories(build_ivf_rabitq_index_d768
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ivf-rabitq
)

add_executable(test_ivf_rabitq_double_oram_d96
    test_ivf_rabitq_double_oram.cpp
)

target_compile_options(test_ivf_rabitq_double_oram_d96
    PRIVATE
        -mavx2
)

target_compile_definitions(test_ivf_rabitq_double_oram_d96
    PRIVATE
        DIM=96
        BB=128
        B_QUERY=4
)

target_link_libraries(test_ivf_rabitq_double_oram_d96
    PRIVATE
        double_oram_library
)

# AnythingLLM's default native embedder (all-MiniLM-L6-v2) emits 384-dimensional
# float vectors. Keep a dedicated compile-time engine because the RaBitQ SIMD
# implementation is parameterized by both the vector and code dimensions.
add_executable(build_ivf_rabitq_index_d384
    ivf-rabitq/build_index.cpp
)

target_compile_options(build_ivf_rabitq_index_d384
    PRIVATE
        -mavx2
)

target_compile_definitions(build_ivf_rabitq_index_d384
    PRIVATE
        DIM=384
        BB=384
        B_QUERY=4
)

target_include_directories(build_ivf_rabitq_index_d384
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ivf-rabitq
)

add_executable(test_ivf_rabitq_double_oram_d384
    test_ivf_rabitq_double_oram.cpp
)

target_compile_options(test_ivf_rabitq_double_oram_d384
    PRIVATE
        -mavx2
)

target_compile_definitions(test_ivf_rabitq_double_oram_d384
    PRIVATE
        DIM=384
        BB=384
        B_QUERY=4
)

target_link_libraries(test_ivf_rabitq_double_oram_d384
    PRIVATE
        double_oram_library
)

add_executable(test_ivf_rabitq_double_oram_d768
    test_ivf_rabitq_double_oram.cpp
)

target_compile_options(test_ivf_rabitq_double_oram_d768
    PRIVATE
        -mavx2
)

target_compile_definitions(test_ivf_rabitq_double_oram_d768
    PRIVATE
        DIM=768
        BB=768
        B_QUERY=4
)

target_link_libraries(test_ivf_rabitq_double_oram_d768
    PRIVATE
        double_oram_library
)

add_executable(test_ivf_rabitq_encrypted_baseline
    test_ivf_rabitq_encrypted_baseline.cpp
)

target_compile_options(test_ivf_rabitq_encrypted_baseline
    PRIVATE
        -mavx2
)

target_link_libraries(test_ivf_rabitq_encrypted_baseline
    PRIVATE
        double_oram_library
)

function(add_search_local_target target_name)
    add_executable(${target_name}
        ivf-rabitq/search_local.cpp
    )
    target_compile_options(${target_name}
        PRIVATE
            -mavx2
    )
    target_compile_definitions(${target_name}
        PRIVATE
            ${ARGN}
    )
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/ivf-rabitq
    )
    target_link_libraries(${target_name}
        PRIVATE
            OpenMP::OpenMP_CXX
            OpenSSL::Crypto
    )
endfunction()

add_search_local_target(search_local DIM=128 BB=128 B_QUERY=4)
add_search_local_target(search_local_d96 DIM=96 BB=128 B_QUERY=4)
add_search_local_target(search_local_d384 DIM=384 BB=384 B_QUERY=4)
add_search_local_target(search_local_d768 DIM=768 BB=768 B_QUERY=4)

if(BUILD_TESTING)
    add_test(NAME co_oram_workflow COMMAND test_double_oram_workflow)
    add_test(NAME co_ann_workflow COMMAND test_ann_double_oram_workflow)
endif()

set(QCOVERT_CO_TEST_PYTHON "" CACHE FILEPATH "Python with NumPy/FAISS for raw-input integration tests")
if(BUILD_TESTING AND QCOVERT_CO_TEST_PYTHON)
    add_test(NAME co_raw_datasets COMMAND "${QCOVERT_CO_TEST_PYTHON}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/raw_dataset_test.py" "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(co_raw_datasets PROPERTIES TIMEOUT 180
        ENVIRONMENT "OMP_NUM_THREADS=1;OPENBLAS_NUM_THREADS=1;PYTHONDONTWRITEBYTECODE=1")
endif()
