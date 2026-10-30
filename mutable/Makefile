# Mutable Instruments modules in one unit file, /units/Mutable.unt: the original module code
# (eurorack/, a git submodule: `git submodule update --init --recursive`) with a wrapper per
# module (<module>.cc). The module code is MIT licensed, see eurorack/README.md.
#
# Modules designed for 72 MHz STM32F1/F3 chips run on both boards. The ones written for
# 168 MHz STM32F4 (or the 400 MHz H7 of Beads) are built for the Raspberry Pi only: the
# BeagleBone's Cortex-A8 does scalar floating point slowly. Moving a module between the lists
# is all it takes to try it on the BeagleBone (measure with the live monitor's CPU view).
#
# `make ONLY="plaits rings"` builds a bundle with just those modules.
NAME := mutable
BUNDLE := Mutable

BOTH_BOARDS := plaits braids tides stages marbles
PI_ONLY := rings clouds elements warps beads

E := eurorack

STMLIB := $(E)/stmlib/dsp/units.cc $(E)/stmlib/dsp/atan.cc $(E)/stmlib/utils/random.cc

SRCS_plaits := plaits.cc $(E)/plaits/resources.cc $(wildcard $(E)/plaits/dsp/*.cc $(E)/plaits/dsp/*/*.cc)
SRCS_braids := braids.cc $(addprefix $(E)/braids/,macro_oscillator.cc analog_oscillator.cc \
		digital_oscillator.cc quantizer.cc resources.cc)
SRCS_tides := tides.cc $(addprefix $(E)/tides2/,poly_slope_generator.cc ramp/ramp_extractor.cc resources.cc)
SRCS_stages := stages.cc $(addprefix $(E)/stages/,segment_generator.cc resources.cc) \
		$(E)/tides2/ramp/ramp_extractor.cc
SRCS_marbles := marbles.cc $(E)/marbles/resources.cc $(E)/marbles/ramp/ramp_extractor.cc \
		$(wildcard $(E)/marbles/random/*.cc)
SRCS_rings := rings.cc $(E)/rings/resources.cc $(wildcard $(E)/rings/dsp/*.cc $(E)/rings/dsp/*/*.cc)
SRCS_clouds := clouds.cc $(E)/clouds/resources.cc $(wildcard $(E)/clouds/dsp/*.cc $(E)/clouds/dsp/*/*.cc)
SRCS_elements := elements.cc $(E)/elements/resources.cc $(wildcard $(E)/elements/dsp/*.cc $(E)/elements/dsp/*/*.cc)
SRCS_warps := warps.cc $(E)/warps/resources.cc $(wildcard $(E)/warps/dsp/*.cc)
SRCS_beads := beads.cc $(E)/beads/resources.cc $(wildcard $(E)/beads/dsp/*.cc $(E)/beads/dsp/*/*.cc)

# Only modules whose wrapper exists (and that ONLY names, if set)
present = $(filter $(basename $(wildcard *.cc)),$(if $(ONLY),$(filter $(ONLY),$(1)),$(1)))
plugins = $(addsuffix _plugin,$(call present,$(1)))
sources = $(foreach m,$(call present,$(1)),$(SRCS_$(m)))

PLUGINS := $(call plugins,$(BOTH_BOARDS))
SRCS := $(call sources,$(BOTH_BOARDS)) $(STMLIB)
PLUGINS_aarch64 := $(call plugins,$(PI_ONLY))
SRCS_aarch64 := $(call sources,$(PI_ONLY))

LIBM := 1
# TEST: the modules' desktop build, without the STM32 drivers (flash user data reads as empty)
# -fwrapv: Braids' fixed-point code counts on signed overflow wrapping (and shifts negative
# numbers left), as it did on the STM32; the host test's sanitizer is told the same.
EXTRA_CXXFLAGS := -I$(E) -I. -DTEST -fno-math-errno -include stdio.h -fwrapv -fno-sanitize=shift-base \
		-Wno-unused-local-typedefs -Wno-unused-variable -Wno-class-memaccess -Wno-unknown-warning-option -Wno-format -Wno-vla-cxx-extension

include ../unit.mk
