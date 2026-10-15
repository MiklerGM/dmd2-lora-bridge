#pragma once
/*
 * What sx1262.h, led.c and the bridge need to know about this board, read from its devicetree
 * rather than written down twice. Included ahead of every source of the app (see CMakeLists.txt),
 * because sx1262.h's defaults are the Heltec's and apply to whatever has not been named first.
 *
 * Pins are port * 32 + pin, the number port_zephyr.c takes apart again.
 */
#include <zephyr/devicetree.h>
#include <zephyr/dt-bindings/gpio/gpio.h>
#include <zephyr/dt-bindings/lora/sx126x.h>

#define BOARD_LORA DT_ALIAS(lora0)
#define BOARD_PSEL(node, prop) \
    (DT_PROP(DT_GPIO_CTLR(node, prop), port) * 32 + DT_GPIO_PIN(node, prop))

#if !DT_NODE_HAS_COMPAT(BOARD_LORA, semtech_sx1262)
#error "this build needs an SX1262 behind the board's lora0 alias"
#endif

#define RADIO_BACKEND_SX1262 1

/* The bus and its chip select come from the devicetree, which port_zephyr.c reads directly;
 * the numbers here are for the startup log only. */
#define PIN_SX_NSS                                                                   \
    (DT_PROP(DT_SPI_DEV_CS_GPIOS_CTLR(BOARD_LORA), port) * 32 +                      \
     DT_SPI_DEV_CS_GPIOS_PIN(BOARD_LORA))
#define PIN_SX_SCK  (-1)
#define PIN_SX_MOSI (-1)
#define PIN_SX_MISO (-1)

#define PIN_SX_RST  BOARD_PSEL(BOARD_LORA, reset_gpios)
#define PIN_SX_BUSY BOARD_PSEL(BOARD_LORA, busy_gpios)
#define PIN_SX_DIO1 BOARD_PSEL(BOARD_LORA, dio1_gpios)
#if DT_NODE_HAS_PROP(BOARD_LORA, rx_enable_gpios)
#define PIN_SX_RXEN BOARD_PSEL(BOARD_LORA, rx_enable_gpios)
#endif

/* No KCT8103L or anything like it: the chip's own output, as on the T-Deck. */
#define PIN_FEM_VCTRL (-1)

/* The driver sets both of these itself, so a board that wires them otherwise has to say so
 * here rather than be driven wrongly. */
#if !DT_PROP(BOARD_LORA, dio2_tx_enable)
#error "the SX1262 backend drives the antenna switch from DIO2"
#endif
#if DT_PROP(BOARD_LORA, dio3_tcxo_voltage) != SX126X_DIO3_TCXO_1V8
#error "the SX1262 backend powers the TCXO at 1.8 V from DIO3"
#endif
#define SX_TCXO_MV 1800

/* The indicator: led.c drives it high for on. */
#if DT_NODE_EXISTS(DT_ALIAS(led0))
#if DT_GPIO_FLAGS(DT_ALIAS(led0), gpios) & GPIO_ACTIVE_LOW
#error "led.c drives the indicator high for on"
#endif
#define PIN_LED BOARD_PSEL(DT_ALIAS(led0), gpios)
#endif

/* Read through port_button_down(), with the polarity and pull the devicetree gives it. */
#if !DT_NODE_EXISTS(DT_ALIAS(sw0))
#error "the bridge needs a button: hold it to forget the paired phone"
#endif

/*
 * Things a board carries that this firmware does not use and should keep from drawing current.
 * The L1's GNSS runs from power-up unless its standby line is pulled low - P1.09, from Seeed's
 * pin map (Meshtastic's variant); Zephyr's board file does not name it.
 */
#if DT_NODE_HAS_COMPAT(DT_ROOT, seeed_wio_tracker_l1)
#define PIN_GNSS_STANDBY (1 * 32 + 9)
#endif
/* Its OLED keeps lit whatever the last firmware drew - Meshtastic's welcome screen, after
 * Meshtastic - since a restart does not reset it. main.c puts it to sleep. */
#if DT_HAS_CHOSEN(zephyr_display) && DT_NODE_HAS_COMPAT(DT_CHOSEN(zephyr_display), sinowealth_sh1106)
#define BOARD_OLED_SH1106 DT_CHOSEN(zephyr_display)
#endif

/* As every ESP32 build: the phone may ask for any of the four levels. */
#ifndef MIN_POWER_INDEX
#define MIN_POWER_INDEX 0
#endif
