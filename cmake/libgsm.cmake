set(LIBGSM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/libgsm")

# Gather sources for libgsm
file(GLOB LIBGSM_SOURCES "${LIBGSM_ROOT}/src/*.c")

# Exclude toast-related files (CLI tool) to keep only the library part
list(REMOVE_ITEM LIBGSM_SOURCES "${LIBGSM_ROOT}/src/toast.c")
list(FILTER LIBGSM_SOURCES EXCLUDE REGEX "toast_.*\\.c$")

add_library(gsm STATIC ${LIBGSM_SOURCES})

target_include_directories(gsm PUBLIC "${LIBGSM_ROOT}/inc")

# SASR (Short Arithmetic Shift Right) is a common requirement for libgsm
target_compile_definitions(gsm PRIVATE SASR)

if(MSVC)
    target_compile_definitions(gsm PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(gsm PRIVATE /wd4244 /wd4267)
endif()

