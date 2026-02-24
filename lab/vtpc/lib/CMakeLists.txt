add_library(
        vtpc
        STATIC
        vtpc.c
)

target_include_directories(
        vtpc
        PUBLIC
        .
)

add_library(
        vtpc_test
        STATIC
        vtpc-test.c
)

target_include_directories(
        vtpc_test
        PUBLIC
        .
)
target_compile_options(vtpc_test PRIVATE -O3 -DNDEBUG)

add_executable(
        io-loader-cache
        io-loader-cache.c
)

target_link_libraries(
        io-loader-cache
        PRIVATE
        vtpc
)

add_executable(
        io-loader-cache-test
        io-loader-cache-test.c
)

target_link_libraries(
        io-loader-cache-test
        PRIVATE
        vtpc_test
)
target_compile_options(io-loader-cache-test PRIVATE -O3 -DNDEBUG)

add_executable(
        io-loader
        io-loader.c
)
