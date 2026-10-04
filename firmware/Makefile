# Host-side checks for code that has no ESP-IDF dependencies.
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


# One flashable file per board, for people who want to run the bridge without a build
# environment: `esptool.py write_flash 0x0 <file>` and nothing else.
#
# The bootloader offset is 0x1000 on the original ESP32 and 0x0 on the S3 and C3. That single
# difference is why this is a case statement and not a loop over three identical commands.
# esptool comes from PlatformIO rather than pip, so it is the same version that built the images.
merged: 
	@mkdir -p dist
	@for e in esp32dev esp32s3 esp32c3; do \
	    case $$e in \
	        esp32dev) chip=esp32;   boot=0x1000 ;; \
	        esp32s3)  chip=esp32s3; boot=0x0 ;; \
	        esp32c3)  chip=esp32c3; boot=0x0 ;; \
	    esac; \
	    pio pkg exec -p tool-esptoolpy -- esptool.py --chip $$chip merge_bin \
	        -o dist/dmd2-lora-bridge-$$e.bin \
	        $$boot .pio/build/$$e/bootloader.bin \
	        0x8000 .pio/build/$$e/partitions.bin \
	        0x10000 .pio/build/$$e/firmware.bin >/dev/null || exit 1; \
	    echo "dist/dmd2-lora-bridge-$$e.bin"; \
	done

.PHONY: test merged
