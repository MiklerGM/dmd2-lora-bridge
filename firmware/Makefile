# Host-side checks for code that has no ESP-IDF dependencies.
test:
	@gcc -std=c11 -Wall -Wextra -Werror -o /tmp/test_e220_regs \
		test/test_e220_regs.c src/e220_regs.c
	@/tmp/test_e220_regs

.PHONY: test
