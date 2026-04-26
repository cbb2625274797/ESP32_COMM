idf_component_register(
    SRCS "cJSON.c"
    INCLUDE_DIRS "."
    REQUIRES
)

# ESP-IDF uses no DLL, export all symbols
target_compile_definitions(${COMPONENT_LIB} PUBLIC CJSON_HIDE_SYMBOLS=0)
