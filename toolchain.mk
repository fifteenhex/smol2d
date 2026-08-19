ifdef CROSS_COMPILER
CC := $(CROSS_COMPILER)gcc
AR := $(CROSS_COMPILER)ar
PKG_CONFIG := $(CROSS_COMPILER)pkg-config

ifeq (,$(shell command -v $(CC) 2>/dev/null))
$(error $(CC) not found)
endif
endif

PKG_CONFIG ?= pkg-config
