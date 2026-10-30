# A unit file's plugins as CLAP, VST3 and AudioUnit plugins for a DAW on the Mac (README.md).
#
#   make UNIT=yafr              # → build/yafr/plugins/metalhead YAFR.{clap,vst3,component}
#   make install UNIT=stock     # build, sign, and copy them to ~/Library/Audio/Plug-Ins
#   make sign UNIT=stock        # sign with the Developer ID in the keychain (else ad hoc)
#
# Needs CMake and Ninja (MacPorts: port install cmake ninja; or pip3 install cmake ninja).

UNIT ?= stock
CMAKE ?= cmake
BUILD := build/$(notdir $(abspath ../$(UNIT)))
PLUGINS_DIR := $(HOME)/Library/Audio/Plug-Ins

# The first "Developer ID Application" identity in the keychain; "-" signs ad hoc (this Mac only).
# SIGN_IDENTITY="Apple Development: ..." picks another one.
SIGN_IDENTITY ?= $(or $(shell security find-identity -v -p codesigning 2>/dev/null | \
		sed -n 's/.*"\(Developer ID Application:[^"]*\)".*/\1/p' | head -1),-)

all:
	$(CMAKE) -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Release -DUNIT=$(UNIT)
	$(CMAKE) --build $(BUILD)

# Hardened runtime and a secure timestamp: what notarization asks for
sign: all
	cd $(BUILD)/plugins && for p in *.clap *.vst3 *.component; do \
		codesign --force --sign "$(SIGN_IDENTITY)" --options runtime --timestamp "$$p" && \
		codesign --verify --strict --verbose=1 "$$p" || exit 1; done

install: sign
	mkdir -p "$(PLUGINS_DIR)/CLAP" "$(PLUGINS_DIR)/VST3" "$(PLUGINS_DIR)/Components"
	cd $(BUILD)/plugins && for p in *.clap; do rm -rf "$(PLUGINS_DIR)/CLAP/$$p"; cp -R "$$p" "$(PLUGINS_DIR)/CLAP/"; done
	cd $(BUILD)/plugins && for p in *.vst3; do rm -rf "$(PLUGINS_DIR)/VST3/$$p"; cp -R "$$p" "$(PLUGINS_DIR)/VST3/"; done
	cd $(BUILD)/plugins && for p in *.component; do rm -rf "$(PLUGINS_DIR)/Components/$$p"; cp -R "$$p" "$(PLUGINS_DIR)/Components/"; done
	killall -9 AudioComponentRegistrar 2>/dev/null || true    # so AU hosts see the new component

clean:
	rm -rf build

.PHONY: all sign install clean
