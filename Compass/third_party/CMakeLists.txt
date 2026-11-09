find_package(OpenSSL REQUIRED)

add_library(rapidjson INTERFACE)
target_include_directories(rapidjson INTERFACE rapidjson/include/)

add_library(argmap INTERFACE)
target_include_directories(argmap INTERFACE argmap/)

add_library(net INTERFACE)
target_include_directories(net INTERFACE net/)

add_library(evp openssl/evp.h openssl/evp.cpp)
target_include_directories(evp PUBLIC openssl/)
target_link_libraries(evp PRIVATE OpenSSL::SSL OpenSSL::Crypto)

if(COMPASS_BUILD_LEGACY_BASELINES)
    set(SEAL_USE_MSGSL OFF CACHE BOOL "Disable optional GSL" )
    set(SEAL_USE_ZLIB OFF CACHE BOOL "Disable optional compression")
    set(SEAL_USE_ZSTD OFF CACHE BOOL "Disable optional compression")
    add_subdirectory(SEAL)
endif()

# Optional upstream Faiss is only used by the legacy HE/cluster baselines.
if(COMPASS_BUILD_LEGACY_BASELINES)
# Build FAISS from pinned sources
set(FAISS_ENABLE_GPU OFF CACHE BOOL "Disable GPU support" FORCE)
set(FAISS_ENABLE_PYTHON OFF CACHE BOOL "Disable Python support" FORCE)
set(BUILD_TESTING OFF)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static library" FORCE)
add_subdirectory(faiss)
endif()
