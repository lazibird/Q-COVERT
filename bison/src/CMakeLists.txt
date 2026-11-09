add_subdirectory(diskann)
add_subdirectory(oram)
add_subdirectory(third_party)

add_library(oram INTERFACE)
target_include_directories(oram INTERFACE oram/)