idf_component_register(
    SRCS "main.c" "cfg_parse.c" "bench_cmd.c" "wire_payload.c" "radio_e220.c" "sx1262.c" "e220.c" "e220_regs.c" "ble_nus.c" "led.c"
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash bt
)
