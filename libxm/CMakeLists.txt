set(LIBXM_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/src")
set(LIBXM_COMPAT_DIR "${CMAKE_CURRENT_LIST_DIR}/compat")

if(CONFIG_XM_SAMPLE_FLOAT)
    set(XM_SAMPLE_TYPE "float")
elseif(CONFIG_XM_SAMPLE_INT16)
    set(XM_SAMPLE_TYPE "int16_t")
elseif(CONFIG_XM_SAMPLE_INT8)
    set(XM_SAMPLE_TYPE "int8_t")
elseif(CONFIG_XM_SAMPLE_DD4A)
    set(XM_SAMPLE_TYPE "dd4a_t")
elseif(CONFIG_XM_SAMPLE_DD8A)
    set(XM_SAMPLE_TYPE "dd8a_t")
else()
    set(XM_SAMPLE_TYPE "int16_t")
endif()

set(XM_SAMPLE_RATE "0" CACHE STRING
    "libxm output sample rate, or 0 to configure at runtime")
set(XM_MICROSTEP_BITS "12" CACHE STRING
    "libxm sample stepping precision, 8..12")
set(XM_PANNING_TYPE "8" CACHE STRING
    "libxm panning type")
set(XM_LOOPING_TYPE "2" CACHE STRING
    "libxm looping type")
set(XM_DISABLED_EFFECTS "0" CACHE STRING
    "libxm disabled effects bit mask")
set(XM_DISABLED_VOLUME_EFFECTS "0" CACHE STRING
    "libxm disabled volume effects bit mask")
set(XM_DISABLED_FEATURES "0" CACHE STRING
    "libxm disabled features bit mask")

option(XM_VERBOSE "libxm verbose logging" OFF)
option(XM_LINEAR_INTERPOLATION "libxm linear interpolation" OFF)
option(XM_RAMPING "libxm ramping" OFF)
option(XM_LIBXM_DELTA_SAMPLES "libxm delta samples in dump format" ON)
option(XM_STRINGS "libxm stores module, instrument, and sample names" ON)
option(XM_TIMING_FUNCTIONS "libxm timing query functions" ON)
option(XM_MUTING_FUNCTIONS "libxm muting functions" ON)
option(XM_MOD_SOFT_PANNING "libxm use 80% panning instead of 100% for MOD files" ON)

configure_file("${LIBXM_SRC_DIR}/xm.h.in" "${CMAKE_CURRENT_BINARY_DIR}/xm.h" @ONLY)

idf_component_register(
    SRCS
        "${LIBXM_SRC_DIR}/xm.c"
        "${LIBXM_SRC_DIR}/load.c"
        "${LIBXM_SRC_DIR}/play.c"
        "${LIBXM_SRC_DIR}/analyze.c"
    INCLUDE_DIRS
        "${CMAKE_CURRENT_BINARY_DIR}"
    PRIV_INCLUDE_DIRS
        "${LIBXM_COMPAT_DIR}"
        "${LIBXM_SRC_DIR}"
    PRIV_REQUIRES
        libsac
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -ffast-math -Wno-error=maybe-uninitialized)
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    XM_VERBOSE=$<BOOL:${XM_VERBOSE}>
    XM_LINEAR_INTERPOLATION=$<BOOL:${XM_LINEAR_INTERPOLATION}>
    XM_RAMPING=$<BOOL:${XM_RAMPING}>
    XM_LIBXM_DELTA_SAMPLES=$<BOOL:${XM_LIBXM_DELTA_SAMPLES}>
    XM_STRINGS=$<BOOL:${XM_STRINGS}>
    XM_TIMING_FUNCTIONS=$<BOOL:${XM_TIMING_FUNCTIONS}>
    XM_MUTING_FUNCTIONS=$<BOOL:${XM_MUTING_FUNCTIONS}>
    XM_MOD_SOFT_PANNING=$<BOOL:${XM_MOD_SOFT_PANNING}>
    XM_SAMPLE_RATE=${XM_SAMPLE_RATE}
    XM_MICROSTEP_BITS=${XM_MICROSTEP_BITS}
    XM_PANNING_TYPE=${XM_PANNING_TYPE}
    XM_LOOPING_TYPE=${XM_LOOPING_TYPE}
    "XM_DISABLED_EFFECTS=${XM_DISABLED_EFFECTS}ULL"
    "XM_DISABLED_VOLUME_EFFECTS=${XM_DISABLED_VOLUME_EFFECTS}ULL"
    "XM_DISABLED_FEATURES=${XM_DISABLED_FEATURES}ULL"
)
if(XM_SAMPLE_TYPE STREQUAL "dd4a_t")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE XM_SAMPLE_TYPE_DD4A=1)
elseif(XM_SAMPLE_TYPE STREQUAL "dd8a_t")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE XM_SAMPLE_TYPE_DD8A=1)
endif()
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
