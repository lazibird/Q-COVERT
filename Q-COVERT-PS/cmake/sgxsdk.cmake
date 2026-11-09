include_guard(GLOBAL)

function(_sgxsdk_fail msg)
    message(FATAL_ERROR "${msg}")
endfunction()

function(_sgxsdk_set_property name value)
    set_property(GLOBAL PROPERTY "${name}" "${value}")
endfunction()

function(_sgxsdk_get_property name out_var)
    get_property(_is_set GLOBAL PROPERTY "${name}" SET)
    if(NOT _is_set)
        _sgxsdk_fail("missing sgx sdk setting '${name}' (call sgx_sdk_init first)")
    endif()
    get_property(_value GLOBAL PROPERTY "${name}")
    set("${out_var}" "${_value}" PARENT_SCOPE)
endfunction()

function(sgx_sdk_get_setting name out_var)
    string(TOUPPER "${name}" _key)
    _sgxsdk_get_property("SGXSDK_${_key}" _value)
    set("${out_var}" "${_value}" PARENT_SCOPE)
endfunction()

function(_sgxsdk_get_compiler_include_dir out_var)
    _sgxsdk_get_property("SGXSDK_COMPILER_INCLUDE_DIR" _sgx_compiler_include_dir)
    if(NOT _sgx_compiler_include_dir)
        foreach(_implicit_dir IN LISTS CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES)
            if(_implicit_dir MATCHES "gcc" OR _implicit_dir MATCHES "clang")
                set(_sgx_compiler_include_dir "${_implicit_dir}")
                break()
            endif()
        endforeach()
    endif()
    set("${out_var}" "${_sgx_compiler_include_dir}" PARENT_SCOPE)
endfunction()

function(_sgxsdk_bool_to_int value out_var)
    if(value)
        string(TOUPPER "${value}" _v)
        if(_v STREQUAL "ON" OR _v STREQUAL "TRUE" OR _v STREQUAL "1" OR _v STREQUAL "YES")
            set("${out_var}" 1 PARENT_SCOPE)
            return()
        endif()
    endif()
    set("${out_var}" 0 PARENT_SCOPE)
endfunction()

function(_sgxsdk_collect_interface_usage target out_prefix)
    foreach(_prop INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES)
        get_target_property(_value "${target}" "INTERFACE_${_prop}")
        if(NOT _value OR _value STREQUAL "INTERFACE_${_prop}-NOTFOUND")
            set(_value)
        endif()
        set("${out_prefix}_${_prop}" "${_value}" PARENT_SCOPE)
    endforeach()
endfunction()

function(sgx_sdk_init)
    set(oneValueArgs SDK_DIR MODE SIGNING_KEY DEBUG PSW_DIR)
    cmake_parse_arguments(SGXSDK "" "${oneValueArgs}" "" ${ARGN})

    if(NOT SGXSDK_SDK_DIR)
        set(SGXSDK_SDK_DIR "$ENV{SGX_SDK}")
    endif()
    if(NOT SGXSDK_SDK_DIR OR NOT EXISTS "${SGXSDK_SDK_DIR}/include/sgx.h")
        _sgxsdk_fail("set SGX_SDK_DIR or SGX_SDK environment variable to a valid Intel SGX SDK root")
    endif()
    file(REAL_PATH "${SGXSDK_SDK_DIR}" SGXSDK_SDK_DIR)

    if(NOT SGXSDK_MODE)
        set(SGXSDK_MODE "HW")
    endif()
    string(TOUPPER "${SGXSDK_MODE}" SGXSDK_MODE)
    set(_sgxsdk_valid_modes HW SIM)
    if(NOT SGXSDK_MODE IN_LIST _sgxsdk_valid_modes)
        _sgxsdk_fail("SGX mode must be HW or SIM (got '${SGXSDK_MODE}')")
    endif()

    if(SGXSDK_SIGNING_KEY)
        if(NOT IS_ABSOLUTE "${SGXSDK_SIGNING_KEY}")
            get_filename_component(SGXSDK_SIGNING_KEY "${SGXSDK_SIGNING_KEY}" ABSOLUTE)
        endif()
        if(NOT EXISTS "${SGXSDK_SIGNING_KEY}")
            _sgxsdk_fail("signing key '${SGXSDK_SIGNING_KEY}' was not found")
        endif()
    endif()

    if(SGXSDK_PSW_DIR)
        if(NOT IS_DIRECTORY "${SGXSDK_PSW_DIR}")
            _sgxsdk_fail("PSW_DIR '${SGXSDK_PSW_DIR}' is not a directory")
        endif()
        set(_sgxsdk_psw_dir "${SGXSDK_PSW_DIR}")
    else()
        find_library(_sgxsdk_psw_urts
            NAMES sgx_urts
            PATHS
                /usr/lib/x86_64-linux-gnu
                /usr/lib
            NO_DEFAULT_PATH
        )
        if(NOT _sgxsdk_psw_urts)
            find_library(_sgxsdk_psw_urts NAMES sgx_urts)
        endif()
        if(_sgxsdk_psw_urts AND _sgxsdk_psw_urts MATCHES "sgxsdk")
            set(_sgxsdk_psw_urts "")
        endif()
        if(_sgxsdk_psw_urts)
            get_filename_component(_sgxsdk_psw_dir "${_sgxsdk_psw_urts}" DIRECTORY)
        else()
            set(_sgxsdk_psw_dir "")
        endif()
    endif()

    if(("${_sgxsdk_psw_dir}" STREQUAL "") AND EXISTS "/usr/lib/x86_64-linux-gnu/libsgx_urts.so")
        set(_sgxsdk_psw_dir "/usr/lib/x86_64-linux-gnu")
    endif()

    _sgxsdk_bool_to_int("${SGXSDK_DEBUG}" SGXSDK_DEBUG_INT)

    find_program(SGXSDK_EDGER8R sgx_edger8r
        PATHS
            "${SGXSDK_SDK_DIR}/bin/x64"
            "${SGXSDK_SDK_DIR}/bin"
        NO_DEFAULT_PATH
    )
    if(NOT SGXSDK_EDGER8R)
        _sgxsdk_fail("unable to locate sgx_edger8r under '${SGXSDK_SDK_DIR}'")
    endif()

    find_program(SGXSDK_SIGN_TOOL sgx_sign
        PATHS
            "${SGXSDK_SDK_DIR}/bin/x64"
            "${SGXSDK_SDK_DIR}/bin"
        NO_DEFAULT_PATH
    )
    if(NOT SGXSDK_SIGN_TOOL)
        _sgxsdk_fail("unable to locate sgx_sign under '${SGXSDK_SDK_DIR}'")
    endif()

    set(SGXSDK_LIB_DIR "${SGXSDK_SDK_DIR}/lib64")
    if(NOT EXISTS "${SGXSDK_LIB_DIR}")
        _sgxsdk_fail("expected library directory '${SGXSDK_LIB_DIR}' was not found")
    endif()

    set(_sgxsdk_compiler_include "")
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -print-file-name=include
            OUTPUT_VARIABLE _sgxsdk_compiler_include
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        execute_process(
            COMMAND "${CMAKE_CXX_COMPILER}" -print-resource-dir
            OUTPUT_VARIABLE _sgxsdk_clang_resource
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        if(_sgxsdk_clang_resource)
            set(_sgxsdk_compiler_include "${_sgxsdk_clang_resource}/include")
        endif()
    endif()
    if(_sgxsdk_compiler_include AND NOT EXISTS "${_sgxsdk_compiler_include}")
        set(_sgxsdk_compiler_include "")
    endif()

    _sgxsdk_set_property("SGXSDK_SDK_DIR" "${SGXSDK_SDK_DIR}")
    _sgxsdk_set_property("SGXSDK_MODE" "${SGXSDK_MODE}")
    _sgxsdk_set_property("SGXSDK_SIGNING_KEY" "${SGXSDK_SIGNING_KEY}")
    _sgxsdk_set_property("SGXSDK_DEBUG" "${SGXSDK_DEBUG}")
    _sgxsdk_set_property("SGXSDK_DEBUG_INT" "${SGXSDK_DEBUG_INT}")
    _sgxsdk_set_property("SGXSDK_EDGER8R" "${SGXSDK_EDGER8R}")
    _sgxsdk_set_property("SGXSDK_SIGN" "${SGXSDK_SIGN_TOOL}")
    _sgxsdk_set_property("SGXSDK_LIB_DIR" "${SGXSDK_LIB_DIR}")
    _sgxsdk_set_property("SGXSDK_PSW_LIB_DIR" "${_sgxsdk_psw_dir}")
    _sgxsdk_set_property("SGXSDK_COMPILER_INCLUDE_DIR" "${_sgxsdk_compiler_include}")
endfunction()

function(sgx_sdk_configure_trusted_target target)
    if(NOT TARGET "${target}")
        _sgxsdk_fail("sgx_sdk_configure_trusted_target: target '${target}' not found")
    endif()

    set(multiValueArgs INCLUDE_DIRS COMPILE_OPTIONS COMPILE_DEFINITIONS)
    cmake_parse_arguments(SGXTRUSTED "" "" "${multiValueArgs}" ${ARGN})

    _sgxsdk_get_property("SGXSDK_SDK_DIR" SGXSDK_SDK_DIR)
    _sgxsdk_get_compiler_include_dir(_sgx_compiler_include_dir)

    set(_include_dirs
        "${SGXSDK_SDK_DIR}/include"
        "${SGXSDK_SDK_DIR}/include/tlibc"
        "${SGXSDK_SDK_DIR}/include/libcxx"
        ${SGXTRUSTED_INCLUDE_DIRS}
    )
    if(_sgx_compiler_include_dir)
        list(INSERT _include_dirs 0 "${_sgx_compiler_include_dir}")
    endif()
    list(REMOVE_DUPLICATES _include_dirs)
    if(_include_dirs)
        target_include_directories("${target}" PRIVATE ${_include_dirs})
    endif()

    if(_sgx_compiler_include_dir)
        target_compile_options("${target}" PRIVATE "-I${_sgx_compiler_include_dir}")
    endif()

    target_compile_options("${target}"
        PRIVATE
            -nostdinc
            $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>
            -fvisibility=hidden
            -fpie
            -ffunction-sections
            -fdata-sections
            -fstack-protector-strong
            $<$<COMPILE_LANGUAGE:CXX>:-include>
            $<$<COMPILE_LANGUAGE:CXX>:${SGXSDK_SDK_DIR}/include/libcxx/__sgx>
            ${SGXTRUSTED_COMPILE_OPTIONS}
    )

    target_compile_definitions("${target}"
        PRIVATE
            __SGXSDK
            __SGXSDK_ENCLAVE
            SGX_TRUSTED
            __LIBCPP_SGX
            _LIBCPP_HAS_NO_THREADS
            _LIBCPP_SGX_CONFIG
            _LIBCPP_HAS_NO_VENDOR_AVAILABILITY_ANNOTATIONS
            _LIBCPP_NO_NATIVE_SEMAPHORES
            _LIBCPP_HAS_NO_GLOBAL_FILESYSTEM_NAMESPACE
            _LIBCPP_HAS_NO_STDIN
            _LIBCPP_HAS_NO_STDOUT
            ${SGXTRUSTED_COMPILE_DEFINITIONS}
    )

    if(TARGET snsgxcxx)
        target_link_libraries("${target}" PRIVATE snsgxcxx)
        target_include_directories("${target}" BEFORE PRIVATE
            $<TARGET_PROPERTY:snsgxcxx,INTERFACE_INCLUDE_DIRECTORIES>
        )
    endif()

    set_target_properties("${target}" PROPERTIES POSITION_INDEPENDENT_CODE ON)
endfunction()

function(sgx_sdk_add_trusted_static_library target)
    set(options)
    set(oneValueArgs)
    set(multiValueArgs
        SOURCES
        PUBLIC_INCLUDE_DIRS
        PRIVATE_INCLUDE_DIRS
        PUBLIC_COMPILE_DEFINITIONS
        PRIVATE_COMPILE_DEFINITIONS
        PUBLIC_COMPILE_OPTIONS
        PRIVATE_COMPILE_OPTIONS
        PUBLIC_LINK_LIBS
        PRIVATE_LINK_LIBS
        DEPENDS
    )
    cmake_parse_arguments(SGXTL "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT SGXTL_SOURCES)
        _sgxsdk_fail("sgx_sdk_add_trusted_static_library(${target}) requires SOURCES")
    endif()

    add_library(${target} STATIC ${SGXTL_SOURCES})

    foreach(_scope IN ITEMS PUBLIC PRIVATE)
        set(_include_values "${SGXTL_${_scope}_INCLUDE_DIRS}")
        if(_include_values)
            target_include_directories(${target} ${_scope} ${_include_values})
        endif()

        set(_define_values "${SGXTL_${_scope}_COMPILE_DEFINITIONS}")
        if(_define_values)
            target_compile_definitions(${target} ${_scope} ${_define_values})
        endif()

        set(_option_values "${SGXTL_${_scope}_COMPILE_OPTIONS}")
        if(_option_values)
            target_compile_options(${target} ${_scope} ${_option_values})
        endif()

        set(_link_values "${SGXTL_${_scope}_LINK_LIBS}")
        if(_link_values)
            target_link_libraries(${target} ${_scope} ${_link_values})
        endif()
    endforeach()

    if(SGXTL_DEPENDS)
        add_dependencies(${target} ${SGXTL_DEPENDS})
    endif()

    sgx_sdk_configure_trusted_target(${target})
endfunction()

function(sgx_sdk_add_trusted_variant interface_target)
    if(NOT TARGET "${interface_target}")
        _sgxsdk_fail("sgx_sdk_add_trusted_variant: target '${interface_target}' not found")
    endif()

    set(oneValueArgs NAME OUT_TARGET)
    set(multiValueArgs
        SOURCES
        PUBLIC_INCLUDE_DIRS
        PRIVATE_INCLUDE_DIRS
        PUBLIC_COMPILE_DEFINITIONS
        PRIVATE_COMPILE_DEFINITIONS
        PUBLIC_COMPILE_OPTIONS
        PRIVATE_COMPILE_OPTIONS
        PUBLIC_LINK_LIBS
        PRIVATE_LINK_LIBS
        DEPENDS
    )
    cmake_parse_arguments(SGXVAR "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT SGXVAR_SOURCES)
        _sgxsdk_fail("sgx_sdk_add_trusted_variant(${interface_target}) requires SOURCES")
    endif()

    if(SGXVAR_NAME)
        set(_trusted_name "${SGXVAR_NAME}")
    else()
        set(_trusted_name "${interface_target}_trusted")
    endif()

    _sgxsdk_collect_interface_usage("${interface_target}" _interface)

    set(_usage_specs
        INCLUDE_DIRS:INCLUDE_DIRECTORIES
        COMPILE_DEFINITIONS:COMPILE_DEFINITIONS
        COMPILE_OPTIONS:COMPILE_OPTIONS
        LINK_LIBS:LINK_LIBRARIES
    )

    set(_trusted_args SOURCES)
    list(APPEND _trusted_args ${SGXVAR_SOURCES})

    foreach(_spec IN LISTS _usage_specs)
        string(REPLACE ":" ";" _parts "${_spec}")
        list(GET _parts 0 _arg_name)
        list(GET _parts 1 _iface_prop)

        set(_iface_var "_interface_${_iface_prop}")
        set(_public_values "${${_iface_var}}")

        set(_extra_public "SGXVAR_PUBLIC_${_arg_name}")
        if(DEFINED ${_extra_public} AND ${_extra_public})
            list(APPEND _public_values ${${_extra_public}})
        endif()
        if(_public_values)
            list(REMOVE_DUPLICATES _public_values)
            list(APPEND _trusted_args PUBLIC_${_arg_name} ${_public_values})
        endif()

        set(_private_values)
        set(_extra_private "SGXVAR_PRIVATE_${_arg_name}")
        if(DEFINED ${_extra_private} AND ${_extra_private})
            set(_private_values ${${_extra_private}})
        endif()
        if(_private_values)
            list(APPEND _trusted_args PRIVATE_${_arg_name} ${_private_values})
        endif()
    endforeach()

    if(SGXVAR_DEPENDS)
        list(APPEND _trusted_args DEPENDS ${SGXVAR_DEPENDS})
    endif()

    sgx_sdk_add_trusted_static_library("${_trusted_name}" ${_trusted_args})

    target_link_libraries("${interface_target}" INTERFACE "${_trusted_name}")
    set_property(TARGET "${interface_target}" PROPERTY SGXSDK_TRUSTED_TARGET "${_trusted_name}")

    if(COMMAND sonic_target_set_platform)
        sonic_target_set_platform("${_trusted_name}" sgx)
    else()
        set_property(TARGET "${_trusted_name}" PROPERTY SONIC_PLATFORM sgx)
    endif()

    if(SGXVAR_OUT_TARGET)
        set("${SGXVAR_OUT_TARGET}" "${_trusted_name}" PARENT_SCOPE)
    endif()
endfunction()

function(sgx_sdk_generate_edl)
    set(oneValueArgs NAME EDL OUTPUT_DIR OUT_TRUSTED_C OUT_TRUSTED_H OUT_UNTRUSTED_C OUT_UNTRUSTED_H OUT_DIR_VAR OUT_TARGET)
    set(multiValueArgs SEARCH_PATHS DEPENDS)
    cmake_parse_arguments(SGXEDL "" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT SGXEDL_NAME)
        _sgxsdk_fail("sgx_sdk_generate_edl requires NAME")
    endif()
    if(NOT SGXEDL_EDL)
        _sgxsdk_fail("sgx_sdk_generate_edl requires EDL")
    endif()

    get_filename_component(_edl_file "${SGXEDL_EDL}" ABSOLUTE)

    if(NOT SGXEDL_OUTPUT_DIR)
        set(SGXEDL_OUTPUT_DIR "${QCOVERT_PS_BINARY_DIR}/generated/${SGXEDL_NAME}")
    endif()
    file(MAKE_DIRECTORY "${SGXEDL_OUTPUT_DIR}")

    _sgxsdk_get_property("SGXSDK_SDK_DIR" SGXSDK_SDK_DIR)
    _sgxsdk_get_property("SGXSDK_EDGER8R" SGXSDK_EDGER8R)

    set(_edl_search_paths
        "${SGXEDL_SEARCH_PATHS}"
        "${QCOVERT_PS_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${SGXSDK_SDK_DIR}/include"
    )
    list(REMOVE_DUPLICATES _edl_search_paths)

    set(_trusted_c "${SGXEDL_OUTPUT_DIR}/${SGXEDL_NAME}_t.c")
    set(_trusted_h "${SGXEDL_OUTPUT_DIR}/${SGXEDL_NAME}_t.h")
    set(_untrusted_c "${SGXEDL_OUTPUT_DIR}/${SGXEDL_NAME}_u.c")
    set(_untrusted_h "${SGXEDL_OUTPUT_DIR}/${SGXEDL_NAME}_u.h")

    set(_search_args)
    foreach(_path IN LISTS _edl_search_paths)
        if(NOT _path)
            continue()
        endif()
        list(APPEND _search_args --search-path "${_path}")
    endforeach()

    set(_edl_dep_scanner "${QCOVERT_PS_SOURCE_DIR}/cmake/sgx_edl_deps.py")
    if(NOT EXISTS "${_edl_dep_scanner}")
        _sgxsdk_fail("expected dependency scanner '${_edl_dep_scanner}' was not found")
    endif()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    set(_dep_scan_args --edl "${_edl_file}")
    foreach(_path IN LISTS _edl_search_paths)
        if(NOT _path)
            continue()
        endif()
        list(APPEND _dep_scan_args --search "${_path}")
    endforeach()

    execute_process(
        COMMAND "${Python3_EXECUTABLE}" "${_edl_dep_scanner}" ${_dep_scan_args} --list
        OUTPUT_VARIABLE _edl_dep_output
        RESULT_VARIABLE _edl_dep_status
    )
    if(NOT _edl_dep_status EQUAL 0)
        _sgxsdk_fail("failed to scan EDL dependencies for ${SGXEDL_NAME} (status ${_edl_dep_status})")
    endif()
    string(REPLACE "\r\n" "\n" _edl_dep_output "${_edl_dep_output}")
    string(REPLACE "\r" "\n" _edl_dep_output "${_edl_dep_output}")
    string(REPLACE "\n" ";" _edl_auto_depends "${_edl_dep_output}")
    list(FILTER _edl_auto_depends EXCLUDE REGEX "^$")
    list(REMOVE_DUPLICATES _edl_auto_depends)

    set(_edl_depfile "${SGXEDL_OUTPUT_DIR}/${SGXEDL_NAME}.edl.d")

    add_custom_command(
        OUTPUT "${_trusted_c}" "${_trusted_h}" "${_untrusted_c}" "${_untrusted_h}"
        COMMAND "${Python3_EXECUTABLE}" "${_edl_dep_scanner}" ${_dep_scan_args} --depfile "${_edl_depfile}" --target "${_trusted_c}"
        COMMAND "${SGXSDK_EDGER8R}" --trusted "${_edl_file}" ${_search_args} --trusted-dir "${SGXEDL_OUTPUT_DIR}"
        COMMAND "${SGXSDK_EDGER8R}" --untrusted "${_edl_file}" ${_search_args} --untrusted-dir "${SGXEDL_OUTPUT_DIR}"
        DEPENDS "${_edl_dep_scanner}" ${_edl_auto_depends} ${SGXEDL_DEPENDS}
        DEPFILE "${_edl_depfile}"
        COMMENT "generating edger8r outputs for ${SGXEDL_NAME}"
        VERBATIM
    )

    set(_edl_target "${SGXEDL_NAME}_edl")
    add_custom_target("${_edl_target}" DEPENDS "${_trusted_c}" "${_trusted_h}" "${_untrusted_c}" "${_untrusted_h}")

    if(SGXEDL_OUT_TARGET)
        set("${SGXEDL_OUT_TARGET}" "${_edl_target}" PARENT_SCOPE)
    endif()
    if(SGXEDL_OUT_TRUSTED_C)
        set("${SGXEDL_OUT_TRUSTED_C}" "${_trusted_c}" PARENT_SCOPE)
    endif()
    if(SGXEDL_OUT_TRUSTED_H)
        set("${SGXEDL_OUT_TRUSTED_H}" "${_trusted_h}" PARENT_SCOPE)
    endif()
    if(SGXEDL_OUT_UNTRUSTED_C)
        set("${SGXEDL_OUT_UNTRUSTED_C}" "${_untrusted_c}" PARENT_SCOPE)
    endif()
    if(SGXEDL_OUT_UNTRUSTED_H)
        set("${SGXEDL_OUT_UNTRUSTED_H}" "${_untrusted_h}" PARENT_SCOPE)
    endif()
    if(SGXEDL_OUT_DIR_VAR)
        set("${SGXEDL_OUT_DIR_VAR}" "${SGXEDL_OUTPUT_DIR}" PARENT_SCOPE)
    endif()
endfunction()

function(sgx_sdk_generate_enclave_config)
    set(oneValueArgs NAME INPUT OUTPUT_DIR OUT_CONFIG OUT_TARGET)
    cmake_parse_arguments(SGXCFG "" "${oneValueArgs}" "" ${ARGN})

    if(NOT SGXCFG_INPUT)
        _sgxsdk_fail("sgx_sdk_generate_enclave_config requires INPUT")
    endif()
    get_filename_component(_input "${SGXCFG_INPUT}" ABSOLUTE)
    if(NOT EXISTS "${_input}")
        _sgxsdk_fail("enclave config source TOML not found: ${_input}")
    endif()

    if(NOT SGXCFG_NAME)
        get_filename_component(SGXCFG_NAME "${_input}" NAME_WE)
    endif()

    if(NOT SGXCFG_OUTPUT_DIR)
        set(SGXCFG_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    endif()
    file(MAKE_DIRECTORY "${SGXCFG_OUTPUT_DIR}")

    set(_output "${SGXCFG_OUTPUT_DIR}/${SGXCFG_NAME}.enclave.config.xml")

    set(_generator "${QCOVERT_PS_SOURCE_DIR}/cmake/sgx_enclave_config.py")
    if(NOT EXISTS "${_generator}")
        _sgxsdk_fail("expected generator script '${_generator}' was not found")
    endif()

    find_package(Python3 COMPONENTS Interpreter REQUIRED)

    set(_target "${SGXCFG_NAME}_enclave_config")

    add_custom_command(
        OUTPUT "${_output}"
        COMMAND "${Python3_EXECUTABLE}" "${_generator}" --input "${_input}" --output "${_output}"
        DEPENDS "${_input}" "${_generator}"
        COMMENT "generating enclave config -> ${_output}"
        VERBATIM
    )

    add_custom_target("${_target}" DEPENDS "${_output}")

    if(SGXCFG_OUT_CONFIG)
        set("${SGXCFG_OUT_CONFIG}" "${_output}" PARENT_SCOPE)
    endif()
    if(SGXCFG_OUT_TARGET)
        set("${SGXCFG_OUT_TARGET}" "${_target}" PARENT_SCOPE)
    endif()
endfunction()

function(sgx_sdk_get_signed_path target out_var)
    get_target_property(_signed "${target}" SGXSDK_SIGNED_PATH)
    if(NOT _signed)
        _sgxsdk_fail("target '${target}' does not expose an SGX signed output")
    endif()
    set("${out_var}" "${_signed}" PARENT_SCOPE)
endfunction()

function(sgx_sdk_get_sign_target target out_var)
    get_target_property(_sign_target "${target}" SGXSDK_SIGN_TARGET)
    if(NOT _sign_target)
        _sgxsdk_fail("target '${target}' does not have an SGX sign helper target")
    endif()
    set("${out_var}" "${_sign_target}" PARENT_SCOPE)
endfunction()

function(sgx_sdk_get_runtime_signed_target target out_var)
    get_target_property(_runtime_target "${target}" SGXSDK_RUNTIME_SIGNED_TARGET)
    if(_runtime_target)
        set("${out_var}" "${_runtime_target}" PARENT_SCOPE)
    else()
        set("${out_var}" "" PARENT_SCOPE)
    endif()
endfunction()

function(_sgxsdk_select_runtime_lib mode out_var)
    if(mode STREQUAL "SIM")
        set("${out_var}" sgx_urts_sim PARENT_SCOPE)
    else()
        set("${out_var}" sgx_urts PARENT_SCOPE)
    endif()
endfunction()

function(_sgxsdk_select_service_lib mode out_var)
    if(mode STREQUAL "SIM")
        set("${out_var}" sgx_uae_service_sim PARENT_SCOPE)
    else()
        set("${out_var}" sgx_uae_service PARENT_SCOPE)
    endif()
endfunction()

function(_sgxsdk_select_trts_lib mode out_var)
    if(mode STREQUAL "SIM")
        set("${out_var}" sgx_trts_sim PARENT_SCOPE)
        set(_service sgx_tservice_sim)
    else()
        set("${out_var}" sgx_trts PARENT_SCOPE)
        set(_service sgx_tservice)
    endif()
    set("${out_var}_SERVICE" "${_service}" PARENT_SCOPE)
endfunction()

function(sgx_sdk_add_enclave target)
    set(options)
    set(oneValueArgs CONFIG CONFIG_TOML SIGNING_KEY OUTPUT_NAME MODE DEBUG EDL_TARGET SIGNED_OUTPUT_VAR VERSION_SCRIPT)
    set(multiValueArgs SOURCES HEADERS INCLUDE_DIRS LINK_LIBS COMPILE_OPTIONS COMPILE_DEFINITIONS BEFORE_INCLUDE_TARGETS)
    cmake_parse_arguments(SGXENC "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT SGXENC_SOURCES)
        _sgxsdk_fail("sgx_sdk_add_enclave(${target}) requires SOURCES")
    endif()

    _sgxsdk_get_property("SGXSDK_MODE" SGXSDK_DEFAULT_MODE)
    _sgxsdk_get_property("SGXSDK_DEBUG_INT" SGXSDK_DEFAULT_DEBUG_INT)
    _sgxsdk_get_property("SGXSDK_LIB_DIR" SGXSDK_LIB_DIR)
    _sgxsdk_get_property("SGXSDK_SDK_DIR" SGXSDK_SDK_DIR)

    if(NOT SGXENC_MODE)
        set(SGXENC_MODE "${SGXSDK_DEFAULT_MODE}")
    else()
        string(TOUPPER "${SGXENC_MODE}" SGXENC_MODE)
    endif()

    if(NOT SGXENC_DEBUG)
        set(SGXENC_DEBUG "${SGXSDK_DEFAULT_DEBUG_INT}")
    else()
        _sgxsdk_bool_to_int("${SGXENC_DEBUG}" SGXENC_DEBUG)
    endif()

    if(NOT SGXENC_SIGNING_KEY)
        _sgxsdk_get_property("SGXSDK_SIGNING_KEY" SGXENC_SIGNING_KEY)
    endif()

    set(_signing_key "")
    set(_signing_key_target "")
    if(SGXENC_SIGNING_KEY)
        set(_signing_key "${SGXENC_SIGNING_KEY}")
        if(NOT IS_ABSOLUTE "${_signing_key}")
            get_filename_component(_signing_key "${_signing_key}" ABSOLUTE)
        endif()
        if(NOT EXISTS "${_signing_key}")
            _sgxsdk_fail("signing key '${_signing_key}' was not found")
        endif()
    else()
        set(_signing_key "${QCOVERT_PS_BINARY_DIR}/${target}_signing_key.pem")
        find_program(_sgxsdk_openssl openssl)
        if(NOT _sgxsdk_openssl)
            _sgxsdk_fail("openssl executable not found; provide SIGNING_KEY or install openssl")
        endif()
        get_filename_component(_key_dir "${_signing_key}" DIRECTORY)
        add_custom_command(
            OUTPUT "${_signing_key}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${_key_dir}"
            COMMAND "${_sgxsdk_openssl}" genrsa -3 -out "${_signing_key}" 3072
            COMMENT "generating SGX signing key at ${_signing_key}"
            VERBATIM
        )
        add_custom_target("${target}_signing_key" DEPENDS "${_signing_key}")
        set(_signing_key_target "${target}_signing_key")
    endif()

    if(SGXENC_CONFIG AND SGXENC_CONFIG_TOML)
        _sgxsdk_fail("sgx_sdk_add_enclave(${target}) accepts CONFIG or CONFIG_TOML, not both")
    endif()

    set(_sgx_config_target "")
    if(SGXENC_CONFIG_TOML)
        sgx_sdk_generate_enclave_config(
            NAME "${target}"
            INPUT "${SGXENC_CONFIG_TOML}"
            OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated"
            OUT_CONFIG SGXENC_CONFIG
            OUT_TARGET _sgx_config_target
        )
    endif()

    if(NOT SGXENC_CONFIG)
        _sgxsdk_fail("sgx_sdk_add_enclave(${target}) requires CONFIG")
    endif()

    _sgxsdk_select_trts_lib("${SGXENC_MODE}" SGXENC_TRTS_LIB)
    set(SGXENC_SERVICE_LIB "${SGXENC_TRTS_LIB_SERVICE}")

    if(NOT SGXENC_VERSION_SCRIPT)
        get_filename_component(_cfg_dir "${SGXENC_CONFIG}" DIRECTORY)
        set(_default_script "${_cfg_dir}/Enclave.lds")
        if(EXISTS "${_default_script}")
            set(SGXENC_VERSION_SCRIPT "${_default_script}")
        endif()
    endif()
    if(SGXENC_VERSION_SCRIPT AND NOT EXISTS "${SGXENC_VERSION_SCRIPT}")
        _sgxsdk_fail("version script '${SGXENC_VERSION_SCRIPT}' not found")
    endif()

    add_library("${target}" SHARED ${SGXENC_SOURCES} ${SGXENC_HEADERS})
    if(SGXENC_EDL_TARGET)
        add_dependencies("${target}" "${SGXENC_EDL_TARGET}")
    endif()
    if(_sgx_config_target)
        add_dependencies("${target}" "${_sgx_config_target}")
    endif()

    if(SGXENC_OUTPUT_NAME)
        set_target_properties("${target}" PROPERTIES OUTPUT_NAME "${SGXENC_OUTPUT_NAME}" PREFIX "" SUFFIX ".so")
    else()
        set_target_properties("${target}" PROPERTIES PREFIX "" SUFFIX ".so")
    endif()

    target_compile_options("${target}"
        PRIVATE
            -nostdinc
            $<$<COMPILE_LANGUAGE:CXX>:-nostdinc++>
            -fvisibility=hidden
            -fpie
            -ffunction-sections
            -fdata-sections
            -fstack-protector-strong
            -include "${SGXSDK_SDK_DIR}/include/libcxx/__sgx"
            ${SGXENC_COMPILE_OPTIONS}
    )

    target_compile_definitions("${target}"
        PRIVATE
            __SGXSDK
            __SGXSDK_ENCLAVE
            SGX_TRUSTED
            __LIBCPP_SGX
            _LIBCPP_HAS_NO_THREADS
            _LIBCPP_SGX_CONFIG
            _LIBCPP_HAS_NO_VENDOR_AVAILABILITY_ANNOTATIONS
            _LIBCPP_NO_NATIVE_SEMAPHORES
            _LIBCPP_HAS_NO_GLOBAL_FILESYSTEM_NAMESPACE
            _LIBCPP_HAS_NO_STDIN
            _LIBCPP_HAS_NO_STDOUT
            ${SGXENC_COMPILE_DEFINITIONS}
    )

    _sgxsdk_get_compiler_include_dir(_sgx_compiler_include_dir)

    set(_include_dirs
        "${SGXSDK_SDK_DIR}/include"
        "${SGXSDK_SDK_DIR}/include/tlibc"
        "${SGXSDK_SDK_DIR}/include/libcxx"
        ${SGXENC_INCLUDE_DIRS}
    )

    if(_sgx_compiler_include_dir)
        list(INSERT _include_dirs 0 "${_sgx_compiler_include_dir}")
        target_compile_options("${target}" PRIVATE "-I${_sgx_compiler_include_dir}")
    endif()
    list(REMOVE_DUPLICATES _include_dirs)
    target_include_directories("${target}" PRIVATE ${_include_dirs})

    target_link_options("${target}"
        PRIVATE
            -nostdlib
            -nodefaultlibs
            -nostartfiles
            -Wl,-z,relro,-z,now,-z,noexecstack
            -Wl,--no-undefined
            -Wl,-Bstatic
            -Wl,-Bsymbolic
            -Wl,-pie,-eenclave_entry
            -Wl,--export-dynamic
            -Wl,--defsym,__ImageBase=0
            -Wl,--gc-sections
    )
    if(SGXENC_VERSION_SCRIPT)
        target_link_options("${target}" PRIVATE "-Wl,--version-script=${SGXENC_VERSION_SCRIPT}")
    endif()

    set(_sgx_link_args
        -Wl,--whole-archive
        "${SGXSDK_LIB_DIR}/lib${SGXENC_TRTS_LIB}.a"
        -Wl,--no-whole-archive
        -Wl,--whole-archive
        "${SGXSDK_LIB_DIR}/libsgx_tcxx.a"
        -Wl,--no-whole-archive
        -Wl,--start-group
        ${SGXENC_LINK_LIBS}
        "${SGXSDK_LIB_DIR}/libsgx_tstdc.a"
        "${SGXSDK_LIB_DIR}/libsgx_tcrypto.a"
        "${SGXSDK_LIB_DIR}/lib${SGXENC_SERVICE_LIB}.a"
        -Wl,--end-group
    )
    target_link_libraries("${target}" PRIVATE ${_sgx_link_args})
    if(TARGET snsgxcxx)
        target_link_libraries("${target}" PRIVATE snsgxcxx)
        target_include_directories("${target}" BEFORE PRIVATE
            $<TARGET_PROPERTY:snsgxcxx,INTERFACE_INCLUDE_DIRECTORIES>
        )
    endif()
    if(SGXENC_BEFORE_INCLUDE_TARGETS)
        foreach(_shim IN LISTS SGXENC_BEFORE_INCLUDE_TARGETS)
            target_link_libraries("${target}" PRIVATE "${_shim}")
            target_include_directories("${target}" BEFORE PRIVATE
                $<TARGET_PROPERTY:${_shim},INTERFACE_INCLUDE_DIRECTORIES>
            )
        endforeach()
    endif()

    get_target_property(_output_name "${target}" OUTPUT_NAME)
    if(NOT _output_name)
        set(_output_name "${target}")
    endif()

    get_target_property(_library_output_dir "${target}" LIBRARY_OUTPUT_DIRECTORY)
    if(NOT _library_output_dir)
        set(_library_output_dir "${CMAKE_LIBRARY_OUTPUT_DIRECTORY}")
    endif()
    if(NOT _library_output_dir)
        set(_library_output_dir "${QCOVERT_PS_BINARY_DIR}")
    endif()

    _sgxsdk_get_property("SGXSDK_SIGN" SGXSDK_SIGN_TOOL)

    set(_signed_output "${_library_output_dir}/${_output_name}.signed.so")
    add_custom_command(
        OUTPUT "${_signed_output}"
        COMMAND "${SGXSDK_SIGN_TOOL}" sign
            -key "${_signing_key}"
            -enclave "$<TARGET_FILE:${target}>"
            -out "${_signed_output}"
            -config "${SGXENC_CONFIG}"
        DEPENDS "${target}" "${SGXENC_CONFIG}" "${_signing_key}"
        COMMENT "signing enclave -> ${_signed_output}"
        VERBATIM
    )
    add_custom_target("${target}_sign" ALL DEPENDS "${_signed_output}")
    add_dependencies("${target}_sign" "${target}")
    if(_sgx_config_target)
        add_dependencies("${target}_sign" "${_sgx_config_target}")
    endif()
    if(_signing_key_target)
        add_dependencies("${target}_sign" "${_signing_key_target}")
    endif()
    if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set(_runtime_signed "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/${_output_name}.signed.so")
        add_custom_command(
            OUTPUT "${_runtime_signed}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_signed_output}" "${_runtime_signed}"
            DEPENDS "${_signed_output}"
            COMMENT "copying enclave -> ${_runtime_signed}"
            VERBATIM
        )
        add_custom_target("${target}_sign_runtime" ALL DEPENDS "${_runtime_signed}")
        add_dependencies("${target}_sign_runtime" "${target}_sign")
        set_property(TARGET "${target}" PROPERTY SGXSDK_RUNTIME_SIGNED_PATH "${_runtime_signed}")
        set_property(TARGET "${target}" PROPERTY SGXSDK_RUNTIME_SIGNED_TARGET "${target}_sign_runtime")
    endif()

    set_property(TARGET "${target}" PROPERTY SGXSDK_SIGNED_PATH "${_signed_output}")
    set_property(TARGET "${target}" PROPERTY SGXSDK_SIGN_TARGET "${target}_sign")
    set_property(TARGET "${target}" PROPERTY SGXSDK_SIGNING_KEY "${_signing_key}")
    if(SGXENC_SIGNED_OUTPUT_VAR)
        set("${SGXENC_SIGNED_OUTPUT_VAR}" "${_signed_output}" PARENT_SCOPE)
    endif()
endfunction()

function(sgx_sdk_add_host target)
    set(options COPY_SIGNED)
    set(oneValueArgs MODE DEBUG EDL_TARGET SIGNED_ENCLAVE)
    set(multiValueArgs SOURCES HEADERS INCLUDE_DIRS LINK_LIBS COMPILE_OPTIONS COMPILE_DEFINITIONS)
    cmake_parse_arguments(SGXHOST "${options}" "${oneValueArgs}" "${multiValueArgs}" ${ARGN})

    if(NOT SGXHOST_SOURCES)
        _sgxsdk_fail("sgx_sdk_add_host(${target}) requires SOURCES")
    endif()

    _sgxsdk_get_property("SGXSDK_MODE" SGXSDK_DEFAULT_MODE)
    _sgxsdk_get_property("SGXSDK_DEBUG_INT" SGXSDK_DEFAULT_DEBUG_INT)
    _sgxsdk_get_property("SGXSDK_LIB_DIR" SGXSDK_LIB_DIR)
    _sgxsdk_get_property("SGXSDK_PSW_LIB_DIR" SGXSDK_PSW_LIB_DIR)

    if(NOT SGXHOST_MODE)
        set(SGXHOST_MODE "${SGXSDK_DEFAULT_MODE}")
    else()
        string(TOUPPER "${SGXHOST_MODE}" SGXHOST_MODE)
    endif()

    if(NOT SGXHOST_DEBUG)
        set(SGXHOST_DEBUG "${SGXSDK_DEFAULT_DEBUG_INT}")
    else()
        _sgxsdk_bool_to_int("${SGXHOST_DEBUG}" SGXHOST_DEBUG)
    endif()

    _sgxsdk_select_runtime_lib("${SGXHOST_MODE}" SGXHOST_URTS_LIB)
    _sgxsdk_select_service_lib("${SGXHOST_MODE}" SGXHOST_SERVICE_LIB)

    _sgxsdk_get_property("SGXSDK_SDK_DIR" SGXSDK_SDK_DIR)

    if(SGXHOST_MODE STREQUAL "HW" AND NOT SGXSDK_PSW_LIB_DIR AND EXISTS "/usr/lib/x86_64-linux-gnu/libsgx_urts.so")
        set(SGXSDK_PSW_LIB_DIR "/usr/lib/x86_64-linux-gnu")
    endif()
    if(SGXHOST_MODE STREQUAL "HW")
        message(STATUS "[sgxsdk] host target '${target}' hw psw='${SGXSDK_PSW_LIB_DIR}'")
    endif()

    add_executable("${target}" ${SGXHOST_SOURCES} ${SGXHOST_HEADERS})
    if(SGXHOST_EDL_TARGET)
        add_dependencies("${target}" "${SGXHOST_EDL_TARGET}")
    endif()

    target_include_directories("${target}"
        PRIVATE
            "${SGXSDK_SDK_DIR}/include"
            ${SGXHOST_INCLUDE_DIRS}
    )

    target_compile_options("${target}" PRIVATE -fPIC ${SGXHOST_COMPILE_OPTIONS})
    target_compile_definitions("${target}"
        PRIVATE
            __SGXSDK
            __SGXSDK_HOST
            ${SGXHOST_COMPILE_DEFINITIONS}
    )

    set(_sgxhost_link_dirs)
    if(SGXHOST_MODE STREQUAL "HW" AND SGXSDK_PSW_LIB_DIR)
        list(APPEND _sgxhost_link_dirs "${SGXSDK_PSW_LIB_DIR}")
    endif()
    list(APPEND _sgxhost_link_dirs "${SGXSDK_LIB_DIR}")
    list(REMOVE_DUPLICATES _sgxhost_link_dirs)
    if(_sgxhost_link_dirs)
        target_link_directories("${target}" PRIVATE ${_sgxhost_link_dirs})
    endif()

    target_link_libraries("${target}"
        PRIVATE
            ${SGXHOST_URTS_LIB}
            ${SGXHOST_SERVICE_LIB}
            pthread
            dl
            ${SGXHOST_LINK_LIBS}
    )

    set_property(TARGET "${target}" PROPERTY RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    set(_sgxhost_rpath_dirs)
    if(SGXHOST_MODE STREQUAL "HW" AND SGXSDK_PSW_LIB_DIR)
        list(APPEND _sgxhost_rpath_dirs "${SGXSDK_PSW_LIB_DIR}")
    endif()
    list(APPEND _sgxhost_rpath_dirs "${SGXSDK_LIB_DIR}")
    list(REMOVE_DUPLICATES _sgxhost_rpath_dirs)
    foreach(_sgxhost_rpath_dir IN LISTS _sgxhost_rpath_dirs)
        target_link_options("${target}" PRIVATE "-Wl,-rpath,${_sgxhost_rpath_dir}")
    endforeach()

    if(SGXHOST_SIGNED_ENCLAVE AND SGXHOST_COPY_SIGNED)
        get_filename_component(_sgxhost_signed_name "${SGXHOST_SIGNED_ENCLAVE}" NAME)
        if(NOT _sgxhost_signed_name)
            set(_sgxhost_signed_name "enclave.signed.so")
        endif()
        add_custom_command(TARGET "${target}" POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${SGXHOST_SIGNED_ENCLAVE}"
                "$<TARGET_FILE_DIR:${target}>/${_sgxhost_signed_name}"
        )
    endif()
endfunction()
