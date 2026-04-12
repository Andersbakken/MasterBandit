add_subdirectory(dawn)

# HarfBuzz 14+ from submodule (for GPU library, not yet in vcpkg)
set(HB_BUILD_SUBSET OFF CACHE BOOL "" FORCE)
set(HB_BUILD_UTILS OFF CACHE BOOL "" FORCE)
set(HB_BUILD_RASTER OFF CACHE BOOL "" FORCE)
set(HB_BUILD_VECTOR OFF CACHE BOOL "" FORCE)
set(HB_BUILD_GPU ON CACHE BOOL "" FORCE)
set(HB_HAVE_FREETYPE OFF CACHE BOOL "" FORCE)
set(HB_HAVE_CAIRO OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GLIB OFF CACHE BOOL "" FORCE)
set(HB_HAVE_ICU OFF CACHE BOOL "" FORCE)
set(HB_HAVE_GRAPHITE2 OFF CACHE BOOL "" FORCE)
add_subdirectory(harfbuzz)

# SheenBidi from submodule. The scratch-memory pool's lock-free stack has an ABA
# race under high thread contention — two threads can share one scratch buffer
# and corrupt each other's BidiChain. Defining SB_CONFIG_DISABLE_SCRATCH_MEMORY
# routes every allocation through malloc (safe, small perf cost). Comment the
# line below to reproduce the upstream bug for reports.
# Upstream bug: https://github.com/Tehreer/SheenBidi/issues/59
set(SB_CONFIG_UNITY OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
add_subdirectory(SheenBidi)
target_compile_definitions(SheenBidi PRIVATE SB_CONFIG_DISABLE_SCRATCH_MEMORY)
