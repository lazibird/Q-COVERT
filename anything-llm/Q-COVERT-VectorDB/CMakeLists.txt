cmake_minimum_required(VERSION 3.20)
project(qcovert_vector_db LANGUAGES CXX)

enable_testing()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(QCOVERT_CORE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/core")

foreach(required_file
        Common/Block.cpp
        Common/Bucket.cpp
        Common/RandomForOram.cpp
        Common/csprng.cpp
        Common/evp.cpp
        ClusterORAM/ClusterORAM.cpp
        RingORAM/RemoteRing.cpp
        RingORAM/RingORAM.cpp
        DoubleORAM/DoubleORAM.cpp)
    if(NOT EXISTS "${QCOVERT_CORE_ROOT}/${required_file}")
        message(FATAL_ERROR "qcovert core is incomplete: missing ${required_file}")
    endif()
endforeach()

find_package(OpenMP REQUIRED)

add_library(qcovert_oram_core STATIC
    ${QCOVERT_CORE_ROOT}/Common/Block.cpp
    ${QCOVERT_CORE_ROOT}/Common/Bucket.cpp
    ${QCOVERT_CORE_ROOT}/Common/RandomForOram.cpp
    ${QCOVERT_CORE_ROOT}/Common/csprng.cpp
    ${QCOVERT_CORE_ROOT}/Common/evp.cpp
    ${QCOVERT_CORE_ROOT}/ClusterORAM/ClusterORAM.cpp
    ${QCOVERT_CORE_ROOT}/RingORAM/RemoteRing.cpp
    ${QCOVERT_CORE_ROOT}/RingORAM/RingORAM.cpp
    ${QCOVERT_CORE_ROOT}/DoubleORAM/DoubleORAM.cpp
)

target_include_directories(qcovert_oram_core
    PUBLIC
        ${QCOVERT_CORE_ROOT}/Common
        ${QCOVERT_CORE_ROOT}/Common/duthomhas
        ${QCOVERT_CORE_ROOT}/ClusterORAM
        ${QCOVERT_CORE_ROOT}/RingORAM
        ${QCOVERT_CORE_ROOT}/DoubleORAM
)

target_link_libraries(qcovert_oram_core PUBLIC OpenMP::OpenMP_CXX crypto)

add_library(qcovert_storage_wire
    src/shutdown_signal.cpp
    src/storage_protocol.cpp
)

target_include_directories(qcovert_storage_wire PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)

add_library(qcovert_storage_protocol src/storage_connection.cpp)

target_include_directories(qcovert_storage_protocol
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(qcovert_storage_protocol PUBLIC qcovert_storage_wire qcovert_oram_core)

add_library(qcovert_remote_ring_server STATIC
    ${QCOVERT_CORE_ROOT}/RingORAM/RemoteRing.cpp
)

target_compile_definitions(qcovert_remote_ring_server PRIVATE QCOVERT_REMOTE_RING_SERVER_ONLY=1)

target_include_directories(qcovert_remote_ring_server
    PUBLIC
        ${QCOVERT_CORE_ROOT}/Common
        ${QCOVERT_CORE_ROOT}/Common/duthomhas
        ${QCOVERT_CORE_ROOT}/RingORAM
)

target_link_libraries(qcovert_remote_ring_server PUBLIC OpenMP::OpenMP_CXX)

add_library(qcovert_query_catalog
    src/collection_catalog.cpp
    src/collection_data_store.cpp
    src/generation_key_store.cpp
    src/generation_manager.cpp
    src/initialization_manager.cpp
    src/ivf_generation_builder.cpp
    src/management_service.cpp
    src/online_vector_codec.cpp
    src/oram_generation_builder.cpp
    src/search_service.cpp
)

target_include_directories(qcovert_query_catalog
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${QCOVERT_CORE_ROOT}/Common
    PRIVATE
        ${QCOVERT_CORE_ROOT}/ivf-rabitq
        ${QCOVERT_CORE_ROOT}/ivf-rabitq/Eigen
)

target_compile_options(qcovert_query_catalog PRIVATE -mavx2)

target_compile_definitions(qcovert_query_catalog
    PRIVATE QCOVERT_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(qcovert_query_catalog
    PUBLIC OpenMP::OpenMP_CXX crypto qcovert_storage_protocol qcovert_oram_core)

add_executable(qcovert_storage
    src/storage_server.cpp
)

target_include_directories(qcovert_storage
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(qcovert_storage
    PRIVATE
        qcovert_storage_wire
        qcovert_remote_ring_server
        pthread
)

add_executable(qcovert_query src/query_server.cpp)

target_include_directories(qcovert_query
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${QCOVERT_CORE_ROOT}/ivf-rabitq
)

target_link_libraries(qcovert_query PRIVATE qcovert_query_catalog pthread)

add_executable(test_collection_catalog tests/test_collection_catalog.cpp)

target_include_directories(test_collection_catalog
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${QCOVERT_CORE_ROOT}/ivf-rabitq
        ${QCOVERT_CORE_ROOT}/ivf-rabitq/Eigen
)

target_compile_options(test_collection_catalog PRIVATE -mavx2)

target_link_libraries(test_collection_catalog PRIVATE qcovert_query_catalog)

add_executable(test_security_recovery tests/test_security_recovery.cpp)

target_include_directories(test_security_recovery
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${QCOVERT_CORE_ROOT}/Common
        ${QCOVERT_CORE_ROOT}/RingORAM
        ${QCOVERT_CORE_ROOT}/ivf-rabitq
)

target_link_libraries(test_security_recovery
    PRIVATE qcovert_query_catalog qcovert_oram_core pthread)

target_compile_options(test_security_recovery PRIVATE -mavx2)

add_executable(test_oram_generation tests/test_oram_generation.cpp)

target_include_directories(test_oram_generation
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${QCOVERT_CORE_ROOT}/Common
        ${QCOVERT_CORE_ROOT}/RingORAM
        ${QCOVERT_CORE_ROOT}/ivf-rabitq
        ${QCOVERT_CORE_ROOT}/ivf-rabitq/Eigen
)

target_compile_definitions(test_oram_generation
    PRIVATE
        QCOVERT_STORAGE_EXECUTABLE="$<TARGET_FILE:qcovert_storage>"
        QCOVERT_QUERY_EXECUTABLE="$<TARGET_FILE:qcovert_query>"
)

target_compile_options(test_oram_generation PRIVATE -mavx2)
target_link_libraries(test_oram_generation PRIVATE qcovert_query_catalog pthread)
add_dependencies(test_oram_generation qcovert_storage qcovert_query)

add_test(NAME collection_catalog COMMAND test_collection_catalog)
add_test(NAME security_recovery COMMAND test_security_recovery)
add_test(NAME oram_generation COMMAND test_oram_generation)
