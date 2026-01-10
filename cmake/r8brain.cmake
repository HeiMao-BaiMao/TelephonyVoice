set(R8BRAIN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/external/r8brain")

# r8brain-free-src is a header-only library.
# We define it as an INTERFACE library so it can be easily linked.

add_library(r8brain INTERFACE)

target_include_directories(r8brain INTERFACE
    "${R8BRAIN_ROOT}"
)

# Note: r8brain requires a C++ compiler and the standard C++ library.
# By default, it uses a built-in FFT (Ooura FFT).
# If you want to use PFFFT or IPP, you can define macros like R8B_PFFFT_DOUBLE
# and add the corresponding source files to your project.
