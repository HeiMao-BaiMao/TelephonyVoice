set(VO_AMRWBENC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/vo-amrwbenc")

file(GLOB VO_AMRWBENC_SOURCES
    "${VO_AMRWBENC_ROOT}/wrapper.c"
    "${VO_AMRWBENC_ROOT}/common/cmnMemory.c"
    "${VO_AMRWBENC_ROOT}/amrwbenc/src/*.c"
)

add_library(vo-amrwbenc STATIC ${VO_AMRWBENC_SOURCES})

target_include_directories(vo-amrwbenc PUBLIC
    "${VO_AMRWBENC_ROOT}"
    "${VO_AMRWBENC_ROOT}/amrwbenc/inc"
    "${VO_AMRWBENC_ROOT}/common/include"
)

# MSVC specific warning suppressions if needed
if(MSVC)
    # Enable optimization /O2 to disable /RTC1 (Stack Frame Runtime Check) which causes crashes in debug
    target_compile_options(vo-amrwbenc PRIVATE /wd4244 /wd4267 /wd4018 /wd4305 /O2)
    target_compile_definitions(vo-amrwbenc PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
