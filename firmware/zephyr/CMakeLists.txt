# The bridge on Zephyr, for the nRF52840 boards. The shared sources are the ESP32 build's own,
# from ../src; what differs is the platform under them (src/port_zephyr.*), the Bluetooth host
# (src/ble_nus.c) and start-up (src/main.c). See docs/BUILDING.md.
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dmd2_lora_bridge)

set(SHARED ${CMAKE_CURRENT_SOURCE_DIR}/../src)

target_sources(app PRIVATE
    src/main.c
    src/port_zephyr.c
    src/ble_nus.c
    src/store.c
    ${SHARED}/bridge.c
    ${SHARED}/sx1262.c
    ${SHARED}/led.c
    ${SHARED}/cfg_parse.c
    ${SHARED}/wire_payload.c
    ${SHARED}/bench_cmd.c
)
target_include_directories(app PRIVATE src ${SHARED})
# The board's pins, from its devicetree, ahead of everything else in the app: sx1262.h's
# defaults are the Heltec's and apply to whatever has not been named first.
target_compile_options(app PRIVATE -include ${CMAKE_CURRENT_SOURCE_DIR}/src/board.h)
