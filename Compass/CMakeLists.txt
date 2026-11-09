cmake_minimum_required(VERSION 3.23.1 FATAL_ERROR)
# cmake_policy(SET CMP0169 OLD)  # Temporary fix for SEAL dependencies
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
project(CompassBaseline LANGUAGES C CXX)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/LocalDependencies.cmake")
include(CTest)
add_compile_definitions(COMPASS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}" COMPASS_REPOSITORY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
option(COMPASS_BUILD_LEGACY_BASELINES "Build upstream lexical and HE cluster baselines" OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--copy-dt-needed-entries")
# set(CMAKE_BUILD_TYPE Debug)

if(COMPASS_BUILD_LEGACY_BASELINES)
    find_package(LAPACK REQUIRED)
    find_package(BLAS REQUIRED)
endif()
find_package(OpenMP REQUIRED)

# Include directories
add_subdirectory(third_party)
add_subdirectory(src)


# Add executable
add_executable(test_compass_ring ./tests/test_compass_ring.cpp ./src/cluster.cpp ./src/node.cpp ./src/graph.cpp ./tests/utils/config_parser.cpp)
target_link_libraries(test_compass_ring PRIVATE compass_faiss oram_library argmap rapidjson)
target_link_libraries(test_compass_ring PRIVATE ${LAPACK_LIBRARIES} ${BLAS_LIBRARIES} )

add_executable(test_compass_accuracy ./tests/test_compass_accuracy.cpp ./src/cluster.cpp ./src/node.cpp ./src/graph.cpp ./tests/utils/config_parser.cpp)
target_link_libraries(test_compass_accuracy PRIVATE compass_faiss oram_library argmap rapidjson)
target_link_libraries(test_compass_accuracy PRIVATE ${LAPACK_LIBRARIES} ${BLAS_LIBRARIES} )

add_executable(test_compass_accuracy_ablation ./tests/test_compass_accuracy_ablation.cpp ./src/cluster.cpp ./src/node.cpp ./src/graph.cpp ./tests/utils/config_parser.cpp)
target_link_libraries(test_compass_accuracy_ablation PRIVATE compass_faiss oram_library argmap rapidjson)
target_link_libraries(test_compass_accuracy_ablation PRIVATE ${LAPACK_LIBRARIES} ${BLAS_LIBRARIES} )

add_executable(test_compass_tp ./tests/test_compass_ring_tp.cpp ./src/cluster.cpp ./src/node.cpp ./src/graph.cpp ./tests/utils/config_parser.cpp)
target_link_libraries(test_compass_tp PRIVATE compass_faiss oram_library argmap rapidjson)
target_link_libraries(test_compass_tp PRIVATE ${LAPACK_LIBRARIES} ${BLAS_LIBRARIES} )

add_executable(compass_init ./tests/eval/compass_init.cpp ./src/cluster.cpp ./src/node.cpp ./src/graph.cpp ./tests/utils/config_parser.cpp)
target_link_libraries(compass_init PRIVATE compass_faiss oram_library argmap rapidjson)
target_link_libraries(compass_init PRIVATE ${LAPACK_LIBRARIES} ${BLAS_LIBRARIES} )

if(COMPASS_BUILD_LEGACY_BASELINES)
add_executable(obi_init ./tests/baseline/lexical_oram/obi_init.cpp ./tests/baseline/lexical_oram/baseline_utils.cpp)
target_link_libraries(obi_init PRIVATE compass_faiss oram_library argmap rapidjson)
target_link_libraries(obi_init PRIVATE ${LAPACK_LIBRARIES} ${BLAS_LIBRARIES} )

add_executable(test_obi ./tests/baseline/lexical_oram/test_obi.cpp ./tests/baseline/lexical_oram/baseline_utils.cpp)
target_link_libraries(test_obi PRIVATE compass_faiss oram_library argmap rapidjson)
target_link_libraries(test_obi PRIVATE ${LAPACK_LIBRARIES} ${BLAS_LIBRARIES} )

add_executable(obi_accuracy ./tests/baseline/lexical_oram/obi_accuracy.cpp ./tests/baseline/lexical_oram/baseline_utils.cpp)
target_link_libraries(obi_accuracy PRIVATE argmap rapidjson evp)

add_executable(test_cluster_search ./tests/baseline/cluster/cluster_search.cpp ./tests/baseline/cluster/cluster_config_parser.cpp)
target_link_libraries(test_cluster_search PRIVATE SEAL::seal argmap rapidjson net faiss OpenMP::OpenMP_CXX)

add_executable(kmeans_cluster ./tests/baseline/cluster/clustering/kmeans_cluster.cpp ./tests/baseline/cluster/cluster_config_parser.cpp)
target_link_libraries(kmeans_cluster PRIVATE faiss argmap rapidjson OpenMP::OpenMP_CXX)

endif()

if(BUILD_TESTING)
    add_executable(compass_repository_paths_test tests/repository_paths_test.cpp tests/utils/config_parser.cpp)
    target_link_libraries(compass_repository_paths_test PRIVATE rapidjson)
    add_test(NAME compass_baseline.repository_paths COMMAND compass_repository_paths_test)
endif()
if(BUILD_TESTING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME compass_baseline.preparation_paths COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/preparation_paths_test.py")
        set_tests_properties(compass_baseline.preparation_paths PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    endif()
endif()
