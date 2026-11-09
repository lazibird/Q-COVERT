cmake_minimum_required(VERSION 3.23.1)
project(QCOVERT LANGUAGES CXX)
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/LocalDependencies.cmake")
include(CTest)

# anything-llm/ is an independent application with its own installation and build.
# Keep its Node services and embedded vector database outside this CMake/CTest graph.
add_subdirectory(Q-COVERT-CO)

option(QCOVERT_BUILD_PS "Build Q-COVERT-PS and its TEE baselines" ON)
if(QCOVERT_BUILD_PS)
    add_subdirectory(Q-COVERT-PS)
endif()

option(QCOVERT_BUILD_COMPASS "Build original Compass and its low-client-storage path" ON)
if(QCOVERT_BUILD_COMPASS)
    add_subdirectory(Compass)
endif()

option(QCOVERT_BUILD_BISON "Include original BiSON integration and adapter tests" ON)
if(QCOVERT_BUILD_BISON)
    add_subdirectory(bison)
endif()

if(BUILD_TESTING AND QCOVERT_CO_TEST_PYTHON)
    add_test(NAME shared_data.tools COMMAND "${QCOVERT_CO_TEST_PYTHON}"
        "${CMAKE_CURRENT_SOURCE_DIR}/data/tests/test_data_tools.py")
    set_tests_properties(shared_data.tools PROPERTIES TIMEOUT 60
        ENVIRONMENT "OMP_NUM_THREADS=1;OPENBLAS_NUM_THREADS=1;PYTHONDONTWRITEBYTECODE=1")
    add_test(NAME experiments.runners COMMAND "${QCOVERT_CO_TEST_PYTHON}"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/tests/test_experiments.py")
    set_tests_properties(experiments.runners PROPERTIES TIMEOUT 60
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endif()
