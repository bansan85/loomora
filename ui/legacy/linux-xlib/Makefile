# Loomora Legacy UI for Linux: plain C (C89 with POSIX) and Xlib, plain executable.
#
#   make                       x86_64 executable   ->  $(BUILD)/loomora-xlib
#   make ARCH=i686             32-bit executable (gcc -m32; needs the i386 libX11, see below)
#   make STATIC_CXX=1          libstdc++ and libgcc linked statically (manylinux2010 build only)
#   make report                needed shared libraries and the highest GLIBC symbol version
#
# The C++11 backend of core/ is compiled here into the static library libloomora.a (same sources
# as core/CMakeLists.txt) and linked into the executable. The only library linked dynamically
# besides libc (and libdl, where dlopen lives before glibc 2.34) is libX11 (-lX11): libXft and
# fontconfig, which draw the text, are opened at run time with dlopen (xftdl.c), so neither their
# development files nor the libraries themselves are needed to build or to start the program.
#
# The C++ runtime (libstdc++, libgcc) is linked DYNAMICALLY by default (STATIC_CXX=0: the
# distribution's own, the modern path). STATIC_CXX=1 links it statically: only the manylinux2010
# container build, the old-glibc baseline target, sets it (tools/build.py linux legacy), because
# CentOS 6 has no libstdc++ new enough for a C++11 backend (fallback "static-cxx-runtime-xlib" of
# validation/fallbacks.json).
#
# The build goes in $(BUILD), by default a subfolder of $HOME (never /tmp, never the source tree).
#
# This Makefile builds with the compiler it finds. The floor of the UI (glibc 2.12, kernel
# 2.6.32) is only reached with a toolchain whose C library is that old (the manylinux2010 image,
# spike S4 / T069): a build on a recent distribution needs that distribution's recent glibc
# (run `make report`).
#
# 32-bit build on Ubuntu / Debian: sudo dpkg --add-architecture i386 && sudo apt update &&
#   sudo apt install gcc-multilib g++-multilib libx11-dev:i386
ARCH        ?= x86_64
CC          ?= gcc
CXX         ?= g++
AR          ?= ar
STATIC_CXX  ?= 0
BUILD       ?= $(HOME)/loomora-build/linux-legacy-$(ARCH)
X11_CFLAGS  ?=
X11_LIBS    ?= -lX11
DL_LIBS     ?= -ldl

CORE        := ../../../core
ifeq ($(ARCH),i686)
M32         := -m32
else ifeq ($(ARCH),x86_64)
M32         :=
else
$(error ARCH must be x86_64 or i686)
endif

CFLAGS      ?= -O2
CXXFLAGS    ?= -O2
UI_CFLAGS   := $(M32) $(CFLAGS) -std=gnu89 -Wall -Wextra -pedantic -Wdeclaration-after-statement \
               -I$(CORE)/include $(X11_CFLAGS)
CORE_CXXFLAGS := $(M32) $(CXXFLAGS) -std=c++11 -Wall -Wextra -pedantic -fvisibility=hidden \
               -fvisibility-inlines-hidden -I$(CORE)/include
LDFLAGS_ALL := $(M32) -Wl,--as-needed
ifeq ($(STATIC_CXX),1)
LDFLAGS_ALL += -static-libstdc++ -static-libgcc
endif

CORE_SRC    := c_api eol greet probe utf8 version
CORE_OBJ    := $(addprefix $(BUILD)/core/,$(addsuffix .o,$(CORE_SRC)))
UI_OBJ      := $(BUILD)/main.o $(BUILD)/probe.o $(BUILD)/xftdl.o $(BUILD)/keys.o
EXE         := $(BUILD)/loomora-xlib

all: $(EXE)

$(EXE): $(UI_OBJ) $(BUILD)/libloomora.a
	$(CXX) -o $@ $(UI_OBJ) $(BUILD)/libloomora.a $(LDFLAGS_ALL) $(LDFLAGS) $(X11_LIBS) $(DL_LIBS)

$(BUILD)/libloomora.a: $(CORE_OBJ)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/core/%.o: $(CORE)/src/%.cpp | $(BUILD)/core
	$(CXX) $(CORE_CXXFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.c probe.h xftdl.h keys.h $(CORE)/include/loomora.h | $(BUILD)
	$(CC) $(UI_CFLAGS) -c -o $@ $<

$(BUILD) $(BUILD)/core:
	mkdir -p $@

# What the executable needs at run time: shared libraries and the highest GLIBC symbol version.
report: $(EXE)
	@echo "shared libraries:"; objdump -p $(EXE) | grep NEEDED
	@echo "highest versions:"; \
	for p in GLIBC GLIBCXX CXXABI GCC; do \
	  v=$$(objdump -T $(EXE) | grep -o "$${p}_[0-9][0-9.]*" | sort -V -u | tail -n 1); \
	  echo "  $${p}: $${v:-none}"; \
	done

clean:
	rm -rf $(BUILD)

.PHONY: all report clean
