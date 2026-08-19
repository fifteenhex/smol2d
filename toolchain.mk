ifdef CROSS_COMPILER
CC := $(CROSS_COMPILER)gcc
AR := $(CROSS_COMPILER)ar
PKG_CONFIG := $(CROSS_COMPILER)pkg-config

ifeq (,$(shell command -v $(CC) 2>/dev/null))
$(error $(CC) not found)
endif
endif

PKG_CONFIG ?= pkg-config

# No pkg-config, or no sdl3 to find with it, means no sdl backend. Cross
# compiling usually has neither.
HAVE_SDL := $(shell $(PKG_CONFIG) --exists sdl3 2>/dev/null && echo y)
