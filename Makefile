# Everything that can be checked without a phone or a radio.
check: check-android check-firmware

check-android:
	@cd android && ./gradlew --console=plain -q :app:testDebugUnitTest :app:lintDebug :app:assembleDebug
	@echo "android: tests + lint pass, apk built"

check-firmware:
	@$(MAKE) -s -C firmware test
	@cd firmware && pio run -s
	@echo "firmware: host tests pass, image built"

# Build a debug APK named after the commit it came from, so which build is on the phone is
# never in doubt.
apk:
	@cd android && ./gradlew --console=plain -q :app:assembleDebug
	@cp android/app/build/outputs/apk/debug/app-debug.apk \
	    dmd-lora-bridge-$$(git rev-parse --short HEAD).apk
	@ls -l dmd-lora-bridge-$$(git rev-parse --short HEAD).apk
	@sha256sum dmd-lora-bridge-$$(git rev-parse --short HEAD).apk

.PHONY: check check-android check-firmware apk
