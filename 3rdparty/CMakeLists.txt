add_subdirectory(dawn)

# ---- libgrapheme (Unicode grapheme cluster support) ----
set(LIBGRAPHEME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libgrapheme)

# Each gen/*.c program produces a corresponding gen/*.h table header.
# gen/util.c is a shared helper linked into each generator.
set(GRAPHEME_GEN_UTIL ${LIBGRAPHEME_DIR}/gen/util.c)

set(GRAPHEME_GENERATORS character word sentence line bidirectional case)
set(GRAPHEME_GEN_OUTPUTS)
foreach(GEN ${GRAPHEME_GENERATORS})
  add_executable(grapheme_gen_${GEN}
    ${LIBGRAPHEME_DIR}/gen/${GEN}.c
    ${GRAPHEME_GEN_UTIL}
  )
  target_include_directories(grapheme_gen_${GEN} PRIVATE ${LIBGRAPHEME_DIR})

  add_custom_command(
    OUTPUT ${LIBGRAPHEME_DIR}/gen/${GEN}.h
    COMMAND grapheme_gen_${GEN} > ${LIBGRAPHEME_DIR}/gen/${GEN}.h
    DEPENDS grapheme_gen_${GEN}
    WORKING_DIRECTORY ${LIBGRAPHEME_DIR}
    COMMENT "Generating libgrapheme ${GEN} tables"
  )
  list(APPEND GRAPHEME_GEN_OUTPUTS ${LIBGRAPHEME_DIR}/gen/${GEN}.h)
endforeach()

# gen/types.h is produced by a separate step — just needs the data files.
# It's #included by src/util.c. The character generator also produces it.
# Actually types.h is a hand-written header in gen/, not generated.

add_library(grapheme STATIC
  ${LIBGRAPHEME_DIR}/src/bidirectional.c
  ${LIBGRAPHEME_DIR}/src/case.c
  ${LIBGRAPHEME_DIR}/src/character.c
  ${LIBGRAPHEME_DIR}/src/line.c
  ${LIBGRAPHEME_DIR}/src/sentence.c
  ${LIBGRAPHEME_DIR}/src/utf8.c
  ${LIBGRAPHEME_DIR}/src/util.c
  ${LIBGRAPHEME_DIR}/src/word.c
)
target_include_directories(grapheme PUBLIC ${LIBGRAPHEME_DIR})
set_target_properties(grapheme PROPERTIES
  C_STANDARD 99
  POSITION_INDEPENDENT_CODE ON
)
add_custom_target(grapheme_gen DEPENDS ${GRAPHEME_GEN_OUTPUTS})
add_dependencies(grapheme grapheme_gen)

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
