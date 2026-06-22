set(LIBSAC_DIR "${CMAKE_CURRENT_LIST_DIR}/src/lib")
set(LIBSAC_INC_DIR "${CMAKE_CURRENT_LIST_DIR}/src/include")

idf_component_register(
    SRCS
        "${LIBSAC_DIR}/saver.cpp"
        "${LIBSAC_DIR}/loader.cpp"
        "${LIBSAC_DIR}/encoder/encode.cpp"
        "${LIBSAC_DIR}/encoder/encode_dd4a.cpp"
        "${LIBSAC_DIR}/encoder/analyzer.cpp"
        "${LIBSAC_DIR}/encoder/encode_dd8a.cpp"
        "${LIBSAC_DIR}/packed_data.cpp"
        "${LIBSAC_DIR}/decoder/decode_dd8a.cpp"
        "${LIBSAC_DIR}/decoder/decode_dd4a.cpp"
        "${LIBSAC_DIR}/decoder/decode.cpp"
        "${LIBSAC_DIR}/quant_lut_dd4a.cpp"
        "${LIBSAC_DIR}/quant_lut_dd8a.cpp"
    INCLUDE_DIRS
        "${LIBSAC_INC_DIR}"
    PRIV_INCLUDE_DIRS
        "${LIBSAC_DIR}"
    REQUIRES
        heap
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -ffast-math)
