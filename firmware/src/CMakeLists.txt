idf_component_register(
    SRCS "main.c" "e220.c" "e220_regs.c" "ble_nus.c" "led.c"
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash bt
)
