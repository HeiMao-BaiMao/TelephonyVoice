set(G711_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/G711_G72x")

# Only G.711 is used. The legacy ADPCM sources have incomplete C prototypes
# and are not required by this target.
add_library(g711-codec STATIC
    "${G711_ROOT}/g711.c"
)

target_include_directories(g711-codec PUBLIC "${G711_ROOT}")

if(MSVC)
    target_compile_definitions(g711-codec PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
