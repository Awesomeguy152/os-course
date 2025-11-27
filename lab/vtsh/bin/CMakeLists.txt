add_executable(
    vtsh
    shell.c
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
add_executable(
    ema-join-sm
    ema-join-sm.c
)

target_include_directories(
    ema-join-sm
    PUBLIC
    .
)

target_link_libraries(
    ema-join-sm
    PRIVATE
    libvtsh
)
add_executable(
    io-loader
    io-loader.c
)

target_include_directories(
    io-loader
    PUBLIC
    .
)

target_link_libraries(
    io-loader
    PRIVATE
    libvtsh
)

# Сборка cpu-sort
add_executable(
    cpu-sort
    cpu-sort.c
)

target_include_directories(
    cpu-sort
    PUBLIC
    .
    ../lib
)

target_link_libraries(
    cpu-sort
    PRIVATE
    libvtsh
)