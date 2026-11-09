add_library(snsgxcxx INTERFACE)

target_include_directories(snsgxcxx
    BEFORE
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_LIST_DIR}/include>
)

target_compile_options(snsgxcxx
    INTERFACE
        $<$<COMPILE_LANGUAGE:CXX>:-include${CMAKE_CURRENT_LIST_DIR}/include/snsgxcxx_libcxx_config.hpp>
)
