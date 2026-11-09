find_package(OpenSSL REQUIRED)

add_library(bison_rapidjson INTERFACE)
target_include_directories(bison_rapidjson INTERFACE rapidjson/include/)

add_library(bison_argmap INTERFACE)
target_include_directories(bison_argmap INTERFACE argmap/)

add_library(bison_net INTERFACE)
target_include_directories(bison_net INTERFACE net/)

add_library(bison_evp openssl/evp.h openssl/evp.cpp)
target_include_directories(bison_evp PUBLIC openssl/)
target_link_libraries(bison_evp PRIVATE OpenSSL::SSL OpenSSL::Crypto)

add_library(bison_tableprinter INTERFACE)
target_include_directories(bison_tableprinter INTERFACE tableprinter/)