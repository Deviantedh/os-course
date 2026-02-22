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

add_executable(
        io_loader_cache
        io-loader-cache.c
)

target_link_libraries(
        io_loader_cache
        PRIVATE
        vtpc
)

add_executable(
        io_loader
        io-loader.c
)