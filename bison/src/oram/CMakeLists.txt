# Build ORAM

find_package(OpenMP REQUIRED)

# set(LIBURING_INCLUDE_DIR /usr/include/liburing)
# set(LIBURING_LIBRARY_DIR /usr/lib)

# include_directories(${LIBURING_INCLUDE_DIR})

set(ORAM_SRC
    Block.cpp
    Bucket.cpp
    csprng.cpp
    FileReader.cpp
    OramRing.cpp 
    position_map.cpp
    OramLeak.cpp 
    OramReadPathEviction.cpp 
    RandomForOram.cpp 
    RemoteServerStorage.cpp 
#     RemoteServerRing.cpp 
    RemoteRing.cpp 
    RemotePath.cpp 
    RemoteLeak.cpp 
    # utils_uring.cpp
)

add_library(bison_oram_library ${ORAM_SRC})
target_include_directories(bison_oram_library PUBLIC duthomhas/)
target_include_directories(bison_oram_library PUBLIC ./)
target_link_libraries(bison_oram_library PRIVATE OpenMP::OpenMP_CXX)
# target_link_libraries(bison_oram_library PRIVATE ${LIBURING_LIBRARY_DIR}/liburing.so)
target_link_libraries(bison_oram_library PUBLIC bison_evp bison_net)
