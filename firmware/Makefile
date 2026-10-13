# Host-side checks, built without ESP-IDF.
test:
	@gcc -std=c11 -Wall -Wextra -Werror -o /tmp/test_ebyte_frame test/test_ebyte_frame.c
	@/tmp/test_ebyte_frame
	@gcc -std=c11 -Wall -Wextra -Werror -o /tmp/test_e220_regs \
		test/test_e220_regs.c src/e220_regs.c
	@/tmp/test_e220_regs
	@gcc -std=c11 -Wall -Wextra -Werror -Isrc -o /tmp/test_bench_cmd \
		test/test_bench_cmd.c src/bench_cmd.c
	@/tmp/test_bench_cmd
	@gcc -std=c11 -Wall -Wextra -Werror -o /tmp/test_led_priority test/test_led_priority.c
	@/tmp/test_led_priority
	@gcc -std=c11 -Wall -Wextra -Werror -o /tmp/test_cfg_parse \
		test/test_cfg_parse.c src/cfg_parse.c
	@/tmp/test_cfg_parse
	@gcc -std=c11 -Wall -Wextra -Werror -o /tmp/test_wire_payload \
		test/test_wire_payload.c src/wire_payload.c
	@/tmp/test_wire_payload
	@gcc -std=c11 -Wall -Wextra -Werror -o /tmp/test_probe test/test_probe.c
	@/tmp/test_probe
	@# The SX1262 backend itself, on ESP-IDF headers left empty: the test defines what it uses.
	@# Sign comparison is waived because ESP-IDF's own build turns that warning off.
	@mkdir -p /tmp/test_sx1262_idf/driver /tmp/test_sx1262_idf/freertos
	@cd /tmp/test_sx1262_idf && touch driver/gpio.h driver/spi_master.h esp_log.h esp_rom_sys.h \
		esp_timer.h freertos/FreeRTOS.h freertos/semphr.h freertos/task.h
	@gcc -std=c11 -Wall -Wextra -Werror -Wno-sign-compare \
		-I/tmp/test_sx1262_idf -o /tmp/test_sx1262 test/test_sx1262.c
	@/tmp/test_sx1262


# The nRF52840 build (Seeed Wio Tracker L1), on Zephyr: zephyr/setup.sh makes the workspace once,
# then this builds zephyr/build/zephyr/zephyr.uf2, the file to copy onto the board's USB drive.
# A bench build: make nrf NRF_FLAGS=-DBENCH_CMD=1
ZEPHYR_WS ?= $(abspath ../../zephyr-ws)
NRF_BOARD ?= wio_tracker_l1
NRF_FLAGS ?=
nrf:
	@test -f $(ZEPHYR_WS)/env.sh || { echo "no Zephyr workspace at $(ZEPHYR_WS): run zephyr/setup.sh"; exit 1; }
	@. $(ZEPHYR_WS)/env.sh && cd $(ZEPHYR_WS) && west build -b $(NRF_BOARD) \
	    -d $(CURDIR)/zephyr/build $(CURDIR)/zephyr -- -DEXTRA_CFLAGS="$(NRF_FLAGS)"
	@ls -l zephyr/build/zephyr/zephyr.uf2

# The bond store's own read (zephyr/src/store.c) on native_sim's simulated flash: ownership rests
# on it, and on the host is the only place a corrupted record can be made to order.
nrf-test:
	@test -f $(ZEPHYR_WS)/env.sh || { echo "no Zephyr workspace at $(ZEPHYR_WS): run zephyr/setup.sh"; exit 1; }
	@mkdir -p zephyr/tests/store/build
	@. $(ZEPHYR_WS)/env.sh && cd $(ZEPHYR_WS) && west build -b native_sim/native/64 \
	    -d $(CURDIR)/zephyr/tests/store/build $(CURDIR)/zephyr/tests/store \
	    >$(CURDIR)/zephyr/tests/store/build/west.log 2>&1 || { cat $(CURDIR)/zephyr/tests/store/build/west.log; exit 1; }
	@out=$$(zephyr/tests/store/build/zephyr/zephyr.exe -flash_in_ram 2>&1); rc=$$?; \
	    echo "$$out" | grep -E "PASS -|FAIL -|Assertion"; exit $$rc

# One flashable file per board, for people who want to run the bridge without a build
# environment: `esptool.py write_flash 0x0 <file>` and nothing else.
#
# The bootloader offset is 0x1000 on the original ESP32 and 0x0 on the S3 and C3. That single
# difference is why this is a case statement and not a loop over identical commands. The Heltec
# and the T-Deck are S3s.
# esptool comes from PlatformIO rather than pip, so it is the same version that built the images.
merged: 
	@mkdir -p dist
	@for e in esp32dev esp32s3 esp32c3 heltec_sx1262 tdeck_sx1262; do \
	    case $$e in \
	        esp32dev) chip=esp32;   boot=0x1000 ;; \
	        esp32c3)  chip=esp32c3; boot=0x0 ;; \
	        *)        chip=esp32s3; boot=0x0 ;; \
	    esac; \
	    pio pkg exec -p tool-esptoolpy -- esptool.py --chip $$chip merge_bin \
	        -o dist/dmd2-lora-bridge-$$e.bin \
	        $$boot .pio/build/$$e/bootloader.bin \
	        0x8000 .pio/build/$$e/partitions.bin \
	        0x10000 .pio/build/$$e/firmware.bin >/dev/null || exit 1; \
	    echo "dist/dmd2-lora-bridge-$$e.bin"; \
	done

.PHONY: test merged nrf nrf-test
