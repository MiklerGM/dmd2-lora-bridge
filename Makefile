# Everything that can be checked without a phone or a radio.
check: check-android check-firmware check-bench

check-android:
	@cd android && ./gradlew --console=plain -q :app:testDebugUnitTest :app:lintDebug :app:assembleDebug
	@echo "android: tests + lint pass, apk built"

check-firmware:
	@$(MAKE) -s -C firmware test
	@cd firmware && pio run -s
	@echo "firmware: host tests pass, image built"

# The parts of the bench that can be wrong without anything looking wrong: the runner's
# scheduler and its readers - an uneven schedule or a dead reader quietly biases every number it
# prints - and the power analyzers' rules for what counts as a reference. No hardware, no pyserial.
check-bench:
	@python3 bench/interleave.py --self-check
	@python3 bench/analyze_power.py --self-check
	@python3 bench/analyze_sdr_power.py --self-check

# Build a debug APK named after the commit it came from, so which build is on the phone is
# never in doubt.
apk:
	@cd android && ./gradlew --console=plain -q :app:assembleDebug
	@cp android/app/build/outputs/apk/debug/app-debug.apk \
	    dmd-lora-bridge-$$(git rev-parse --short HEAD).apk
	@ls -l dmd-lora-bridge-$$(git rev-parse --short HEAD).apk
	@sha256sum dmd-lora-bridge-$$(git rev-parse --short HEAD).apk

# Signed build for handing to other riders. Needs android/keystore.properties.
release:
	@test -f android/keystore.properties || \
	    { echo "android/keystore.properties missing - cannot sign"; exit 1; }
	@cd android && ./gradlew --console=plain -q :app:assembleRelease
	@cp android/app/build/outputs/apk/release/app-release.apk \
	    dmd-lora-bridge-$$(git rev-parse --short HEAD)-release.apk
	@ls -l dmd-lora-bridge-$$(git rev-parse --short HEAD)-release.apk
	@sha256sum dmd-lora-bridge-$$(git rev-parse --short HEAD)-release.apk

.PHONY: check check-android check-firmware check-bench apk release
