cmake_minimum_required(VERSION 3.22.1 FATAL_ERROR)
project(secure_diskann)
include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/LocalDependencies.cmake")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
include(CTest)
set(BISON_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
add_compile_definitions(BISON_SOURCE_DIR_PATH="${CMAKE_CURRENT_SOURCE_DIR}" BISON_REPOSITORY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/..")
option(BISON_BUILD_NATIVE "Build full BiSON/DiskANN binaries (requires MKL, Boost, AIO, io_uring)" OFF)

if(BISON_BUILD_NATIVE)
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--copy-dt-needed-entries")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mavx2 -mfma -msse2 -ftree-vectorize -Ofast -fopenmp -fopenmp-simd -funroll-loops -Wfatal-errors -DUSE_AVX2")

include_directories(${BISON_SOURCE_DIR}/src)

find_package(Boost REQUIRED COMPONENTS program_options)

# Add Intel MKL and OMP library paths (pip-installed into the venv)
link_directories(${BISON_SOURCE_DIR}/venv/lib)

include_directories(${BISON_SOURCE_DIR}/src/diskann/include)
add_subdirectory(src)

add_executable(client 
    ./src/client.cpp 
    ./src/oram/config_parser.cpp 
    ./src/oram/OramAPI.cpp 
    ./src/diskann/oram_index.cpp
)
target_include_directories(client PRIVATE ${BISON_SOURCE_DIR}/src/diskann ${BISON_SOURCE_DIR}/src/diskann/include)
target_link_libraries(client PRIVATE bison_oram_library bison_argmap bison_rapidjson Boost::program_options diskann aio)

add_executable(server 
    ./src/server.cpp 
    ./src/oram/config_parser.cpp 
    ./src/oram/OramAPI.cpp
)
target_link_libraries(server PRIVATE bison_oram_library bison_argmap bison_rapidjson)

add_executable(oram_initializer 
    ./src/oram/oram_initialization.cpp 
    ./src/oram/OramAPI.cpp 
    ./src/oram/config_parser.cpp 
    ./src/oram/node.cpp
)
target_include_directories(oram_initializer PRIVATE ${BISON_SOURCE_DIR}/src/diskann ${BISON_SOURCE_DIR}/src/diskann/include)
target_link_libraries(oram_initializer PRIVATE bison_oram_library bison_argmap bison_rapidjson Boost::program_options diskann aio)

add_executable(oram_initializer_low
    ./src/oram/oram_initializer_low.cpp
    ./src/oram/config_parser.cpp
)
target_link_libraries(oram_initializer_low PRIVATE bison_oram_library bison_argmap bison_rapidjson)



find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED liburing)

include_directories(${LIBURING_INCLUDE_DIRS})
link_directories(${LIBURING_LIBRARY_DIRS})

# For your 'server' executable target that needs liburing
target_include_directories(server PRIVATE ${LIBURING_INCLUDE_DIRS})
target_link_libraries(server PRIVATE bison_oram_library bison_argmap bison_rapidjson ${LIBURING_LIBRARIES})

# If your client or oram_initializer also use liburing, add similarly:
# For example, if client uses io_uring:
target_include_directories(client PRIVATE ${LIBURING_INCLUDE_DIRS})
target_link_libraries(client PRIVATE bison_oram_library bison_argmap bison_rapidjson Boost::program_options diskann aio ${LIBURING_LIBRARIES})




# NOTE: compute_mrr_compass.cpp and compute_mrr_gt.cpp are referenced but not
# present in the repo (missing from the authors' release). Disabled.
# add_executable(compute_mrr_compass ./compute_mrr_compass.cpp)
# target_include_directories(compute_mrr_compass PRIVATE ${BISON_SOURCE_DIR}/src/diskann ${BISON_SOURCE_DIR}/src/diskann/include)
# target_link_libraries(compute_mrr_compass PRIVATE diskann aio)

# add_executable(compute_mrr_gt ./compute_mrr_gt.cpp)
# target_include_directories(compute_mrr_gt PRIVATE ${BISON_SOURCE_DIR}/src/diskann ${BISON_SOURCE_DIR}/src/diskann/include)
# target_link_libraries(compute_mrr_gt PRIVATE diskann aio)

message(STATUS "C++ compiler: ${CMAKE_CXX_COMPILER}")
message(STATUS "C compiler:   ${CMAKE_C_COMPILER}")

message(STATUS "Build type:   ${CMAKE_BUILD_TYPE}")

message(STATUS "C++ flags:    ${CMAKE_CXX_FLAGS}")
message(STATUS "C flags:      ${CMAKE_C_FLAGS}")

message(STATUS "C++ Debug:    ${CMAKE_CXX_FLAGS_DEBUG}")
message(STATUS "C++ Release:  ${CMAKE_CXX_FLAGS_RELEASE}")
message(STATUS "C++ RelWithDebInfo: ${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
message(STATUS "C++ MinSizeRel:     ${CMAKE_CXX_FLAGS_MINSIZEREL}")

endif()

if(BUILD_TESTING)
    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME bison_baseline.benchmark COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_benchmark.py")
        set_tests_properties(bison_baseline.benchmark PROPERTIES ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
    endif()
    find_package(OpenSSL REQUIRED)
    find_package(OpenMP REQUIRED)
    add_executable(bison_baseline_dataset_adapter tests/test_dataset_adapter.cpp)
    target_include_directories(bison_baseline_dataset_adapter PRIVATE src src/diskann/include src/third_party/argmap src/third_party/rapidjson/include src/oram src/third_party/openssl src/third_party/net src/oram/duthomhas)
    target_link_libraries(bison_baseline_dataset_adapter PRIVATE OpenSSL::Crypto OpenMP::OpenMP_CXX)
    target_compile_options(bison_baseline_dataset_adapter PRIVATE -UNDEBUG)
    add_test(NAME bison_baseline.dataset_adapter COMMAND bison_baseline_dataset_adapter)
endif()
if(BUILD_TESTING)
    add_executable(bison_baseline_config_paths tests/config_paths_test.cpp src/oram/config_parser.cpp)
    target_include_directories(bison_baseline_config_paths PRIVATE src/oram src/third_party/rapidjson/include)
    add_test(NAME bison_baseline.config_paths COMMAND bison_baseline_config_paths)
endif()
