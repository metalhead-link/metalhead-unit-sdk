# A unit file's plugins as CLAP, VST3 and AudioUnit plugins for a DAW on the Mac (README.md).
#
#   make UNIT=yafr              # → build/yafr/plugins/metalhead YAFR.{clap,vst3,component}
#   make install UNIT=stock     # build, and copy them to ~/Library/Audio/Plug-Ins
#
# Needs CMake and Ninja (MacPorts: port install cmake ninja; or pip3 install cmake ninja).

UNIT ?= stock
CMAKE ?= cmake
BUILD := build/$(notdir $(abspath ../$(UNIT)))
PLUGINS_DIR := $(HOME)/Library/Audio/Plug-Ins

all:
	$(CMAKE) -S . -B $(BUILD) -G Ninja -DCMAKE_BUILD_TYPE=Release -DUNIT=$(UNIT)
	$(CMAKE) --build $(BUILD)

install: all
	mkdir -p "$(PLUGINS_DIR)/CLAP" "$(PLUGINS_DIR)/VST3" "$(PLUGINS_DIR)/Components"
	cd $(BUILD)/plugins && for p in *.clap; do rm -rf "$(PLUGINS_DIR)/CLAP/$$p"; cp -R "$$p" "$(PLUGINS_DIR)/CLAP/"; done
	cd $(BUILD)/plugins && for p in *.vst3; do rm -rf "$(PLUGINS_DIR)/VST3/$$p"; cp -R "$$p" "$(PLUGINS_DIR)/VST3/"; done
	cd $(BUILD)/plugins && for p in *.component; do rm -rf "$(PLUGINS_DIR)/Components/$$p"; cp -R "$$p" "$(PLUGINS_DIR)/Components/"; done
	killall -9 AudioComponentRegistrar 2>/dev/null || true    # so AU hosts see the new component

clean:
	rm -rf build

.PHONY: all install clean
