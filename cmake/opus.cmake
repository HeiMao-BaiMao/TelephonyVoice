# ---------------------------------------------------------------------------
# Opus integration (BSD-licensed).
#
# Upstream Opus ships its own CMakeLists.txt. We pull it in via
# add_subdirectory() with options that drop test programs, demos, docs,
# and installation of pkg-config / CMake package config modules. The
# submodule contents are not edited - we only configure the upstream
# project via CMake options and expose the resulting `opus` target as
# `opus-codec` for our build graph.
#
# Options we force:
#   OPUS_BUILD_TESTING=OFF      (no test_opus_* executables)
#   OPUS_BUILD_PROGRAMS=OFF     (no opus_demo / opus_compare)
#   OPUS_INSTALL_PKG_CONFIG_MODULE=OFF
#   OPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF
#   OPUS_BUILD_SHARED_LIBRARY=OFF (we want a static lib that links into the
#                                  plugin)
# ---------------------------------------------------------------------------

# Force options on the upstream project before adding its subdirectory.
# set(... CACHE BOOL ... FORCE) wins over the upstream default.
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
# Fixed-point is the wrong path for this plugin (we want the float API
# and the float internals). Honour the upstream default; force OFF to
# be defensive in case a parent scope flips BUILD_SHARED_LIBS.
set(OPUS_FIXED_POINT OFF CACHE BOOL "" FORCE)

# Note: external/opus/CMakeLists.txt calls `feature_summary(WHAT ALL)` at the
# end of its own configure step. CMake's FeatureSummary module does not
# expose a variable to silence that call (the standard `FeatureSummary`
# options -- FATAL_ON_MISSING_REQUIRED / QUIET_ON_EMPTY / etc. -- only
# affect missing-package reporting). We leave the call alone: trying to
# override a non-existent knob would be a no-op anyway, and editing the
# upstream submodule is out of scope.

add_subdirectory(external/opus)

# The upstream CMakeLists.txt creates target `opus` (and alias `Opus::opus`).
# Add a stable alias so our build graph doesn't depend on the upstream name.
add_library(opus-codec ALIAS opus)