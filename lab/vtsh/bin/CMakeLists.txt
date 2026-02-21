add_executable(
        vtsh
        main.c
        proc-clone3.c
)

add_executable(
        shell
        main.c
        proc-clone3.c
)

add_executable(
        io-loader
        io-loader.c
)

add_executable(
        cpu-linreg
        cpu-linreg.c
)

add_executable(
        ema-traverse-graph
        ema-traverse-graph.c
)

target_include_directories(
        vtsh
        PUBLIC
        .
)

target_link_libraries(
        vtsh
        PRIVATE
        libvtsh
)

target_include_directories(
        shell
        PUBLIC
        .
)

target_link_libraries(
        shell
        PRIVATE
        libvtsh
)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -g -pg")
set(CMAKE_C_COMPILER gcc)
set(CMAKE_C_STANDARD 11)