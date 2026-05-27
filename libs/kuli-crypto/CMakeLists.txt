# kuli-crypto — sha256/md5 over OpenSSL EVP. Leaf module (+ OpenSSL).
add_library(kuli-crypto STATIC
    src/hash.cpp
    src/sign.cpp)
add_library(kuli::crypto ALIAS kuli-crypto)

target_include_directories(kuli-crypto PUBLIC include)
target_compile_features(kuli-crypto PUBLIC cxx_std_23)
target_link_libraries(kuli-crypto
    PUBLIC OpenSSL::Crypto
    PRIVATE kuli-build-flags)
