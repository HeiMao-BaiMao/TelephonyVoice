set(G711_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/G711_G72x")

# Target for G.711, G.721, G.723 codecs
add_library(g711-codec STATIC
    "${G711_ROOT}/g711.c"
    "${G711_ROOT}/g72x.c"
    "${G711_ROOT}/g721.c"
    "${G711_ROOT}/g723_24.c"
    "${G711_ROOT}/g723_40.c"
)

target_include_directories(g711-codec PUBLIC "${G711_ROOT}")

if(MSVC)
    target_compile_definitions(g711-codec PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
