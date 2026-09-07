# Everything that can be checked without a phone or a radio.
check: check-android check-firmware

check-android:
	@cd android && ./gradlew --console=plain -q :app:testDebugUnitTest :app:lintDebug :app:assembleDebug
	@echo "android: tests + lint pass, apk built"

check-firmware:
	@$(MAKE) -s -C firmware test
	@cd firmware && pio run -s
	@echo "firmware: host tests pass, image built"

.PHONY: check check-android check-firmware
