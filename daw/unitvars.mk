# Read after a unit's Makefile, so CMakeLists.txt builds what that Makefile builds:
#   make -s -C ../yafr -f Makefile -f ../daw/unitvars.mk unitvars
unitvars:
	@echo 'NAME=$(NAME)'
	@echo 'BUNDLE=$(BUNDLE)'
	@echo 'PLUGINS=$(strip $(PLUGINS))'
	@echo 'SRCS=$(strip $(SRCS))'

.PHONY: unitvars
