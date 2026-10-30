# All the example plugins in one unit file, /units/Stock.unt: upload this one instead of the
# separate VUMeter, Drums, YAFR and ClipGrid files (a plugin can only be loaded once, so a
# card with both loads whichever file comes first and refuses the other).
NAME := stock
BUNDLE := Stock
PLUGINS := vumeter_plugin drums_plugin yafr_plugin clipgrid_plugin
SRCS := ../vumeter/vumeter.c ../drums/drums.c ../yafr/yafr.c ../clipgrid/clipgrid.c

include ../unit.mk
