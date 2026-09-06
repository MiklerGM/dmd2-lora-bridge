idf_component_register(
    SRCS "main.c" "e220.c" "ble_nus.c"
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash bt
)
