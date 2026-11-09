set(BISON_DIM 128 CACHE STRING "BiSON benchmark embedding dimension")
set(BISON_DEGREE 128 CACHE STRING "BiSON Vamana fixed neighbor slots")
set(BISON_PQ_BYTES 16 CACHE STRING "BiSON PQ code bytes (8-bit subquantizers)")
set(BISON_SCALAR float32 CACHE STRING "BiSON stored embedding: uint8 or float32")
# Extra native-only layouts compiled into the same build tree, e.g.
# -DRINGDORAM_BISON_LAYOUTS="128:128:17:float32;96:128:16:float32;128:128:32:uint8"
# produces bison_benchmark_d128_r128_p17_float32 etc. alongside bison_benchmark.
# Reconfiguring with a longer list adds binaries; earlier binaries stay on disk.
set(RINGDORAM_BISON_LAYOUTS "" CACHE STRING "BiSON extra native layouts: semicolon list of dim:degree:pq_bytes:scalar")

function(bison_layout_definitions dim degree pq scalar out_var)
  if(scalar STREQUAL "uint8")
    set(scalar_id 2)
  elseif(scalar STREQUAL "float32")
    set(scalar_id 1)
  else()
    message(FATAL_ERROR "BiSON scalar must be uint8 or float32, got '${scalar}'")
  endif()
  set(${out_var} BISON_DIM=${dim} BISON_DEGREE=${degree}
      BISON_PQ_BYTES=${pq} BISON_SCALAR_ID=${scalar_id} PARENT_SCOPE)
endfunction()

bison_layout_definitions(${BISON_DIM} ${BISON_DEGREE} ${BISON_PQ_BYTES} ${BISON_SCALAR} BISON_DEFINITIONS)
set(BISON_DEFINITIONS ${BISON_DEFINITIONS} PARENT_SCOPE)

function(bison_layout_library name dim degree pq scalar)
  bison_layout_definitions(${dim} ${degree} ${pq} ${scalar} definitions)
  add_library(${name} INTERFACE)
  target_include_directories(${name} INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
  target_link_libraries(${name} INTERFACE RingDORAM::qcovert)
  target_compile_definitions(${name} INTERFACE ${definitions})
  target_compile_options(${name} INTERFACE $<$<CXX_COMPILER_ID:GNU,Clang>:-ffp-contract=off>)
endfunction()

bison_layout_library(bison_do ${BISON_DIM} ${BISON_DEGREE} ${BISON_PQ_BYTES} ${BISON_SCALAR})
if(RINGDORAM_BUILD_EXAMPLES)
  add_executable(bison_benchmark native.cpp)
  target_link_libraries(bison_benchmark PRIVATE bison_do)
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
      continue() # same as the default bison_benchmark
    endif()
    bison_layout_library(bison_do_d${layout_dim}_r${layout_degree}_p${layout_pq}_${layout_scalar}
        ${layout_dim} ${layout_degree} ${layout_pq} ${layout_scalar})
    add_executable(bison_benchmark_d${layout_dim}_r${layout_degree}_p${layout_pq}_${layout_scalar} native.cpp)
    target_link_libraries(bison_benchmark_d${layout_dim}_r${layout_degree}_p${layout_pq}_${layout_scalar}
        PRIVATE bison_do_d${layout_dim}_r${layout_degree}_p${layout_pq}_${layout_scalar})
  endforeach()
endif()
if(RINGDORAM_BUILD_TESTS)
  add_executable(bison_test tests.cpp)
  target_link_libraries(bison_test PRIVATE bison_do)
  add_test(NAME bison.correctness COMMAND bison_test)
  if(RINGDORAM_ENABLE_SANITIZERS)
    set_tests_properties(bison.correctness PROPERTIES ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
  endif()
  if(RINGDORAM_BENCHMARK_PYTHON AND RINGDORAM_BUILD_EXAMPLES)
    add_test(NAME bison.dataio COMMAND "${RINGDORAM_BENCHMARK_PYTHON}"
      "${CMAKE_CURRENT_SOURCE_DIR}/dataio_test.py")
    add_test(NAME bison.pipeline COMMAND "${RINGDORAM_BENCHMARK_PYTHON}"
      "${CMAKE_CURRENT_SOURCE_DIR}/pipeline_test.py" "${QCOVERT_PS_BINARY_DIR}")
    set_tests_properties(bison.pipeline PROPERTIES ENVIRONMENT "OMP_NUM_THREADS=1;OPENBLAS_NUM_THREADS=1;ASAN_OPTIONS=detect_leaks=0" TIMEOUT 180)
  endif()
endif()
