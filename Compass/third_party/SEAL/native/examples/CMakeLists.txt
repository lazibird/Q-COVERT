# Copyright (c) Microsoft Corporation. All rights reserved.
# Licensed under the MIT license.

cmake_minimum_required(VERSION 3.13)

project(SEALExamples VERSION 4.1.1 LANGUAGES CXX)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${OpenMP_C_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${OpenMP_CXX_FLAGS}")

# If not called from root CMakeLists.txt
if(NOT DEFINED SEAL_BUILD_EXAMPLES)
    set(SEAL_BUILD_EXAMPLES ON)

    # Import Microsoft SEAL
    find_package(SEAL 4.1.1 EXACT REQUIRED)

    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/bin)
endif()

if(SEAL_BUILD_EXAMPLES)
    add_executable(sealexamples)
    add_executable(cluster_search)

    target_sources(sealexamples
        PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}/examples.cpp
            ${CMAKE_CURRENT_LIST_DIR}/1_bfv_basics.cpp
            ${CMAKE_CURRENT_LIST_DIR}/2_encoders.cpp
            ${CMAKE_CURRENT_LIST_DIR}/3_levels.cpp
            ${CMAKE_CURRENT_LIST_DIR}/4_bgv_basics.cpp
            ${CMAKE_CURRENT_LIST_DIR}/5_ckks_basics.cpp
            ${CMAKE_CURRENT_LIST_DIR}/6_rotation.cpp
            ${CMAKE_CURRENT_LIST_DIR}/7_serialization.cpp
            ${CMAKE_CURRENT_LIST_DIR}/8_performance.cpp
    )

    find_package(OpenMP REQUIRED)

    target_sources(cluster_search
        PRIVATE
            ${CMAKE_CURRENT_LIST_DIR}/search/cluster_search.cpp
    )

    # Link OpenMP
    target_link_libraries(cluster_search PUBLIC OpenMP::OpenMP_CXX)

    if(TARGET SEAL::seal)
        target_link_libraries(sealexamples PRIVATE SEAL::seal)
        target_link_libraries(cluster_search PRIVATE SEAL::seal)
    elseif(TARGET SEAL::seal_shared)
        target_link_libraries(sealexamples PRIVATE SEAL::seal_shared)
        target_link_libraries(cluster_search PRIVATE SEAL::seal_shared)
    else()
        message(FATAL_ERROR "Cannot find target SEAL::seal or SEAL::seal_shared")
    endif()
endif()
