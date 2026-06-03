# 3DO-OMF2097 port build
#
# Adapted from Trapexit's 3do-devkit Makefile template — kept project-local
# so the user's shared ~/3do-devkit install stays untouched.
#
# Invoke from inside the devkit environment (build.ps1 handles this).

NAME       = omf2097
ISONAME    = iso/$(NAME).iso
FILESYSTEM = takeme
LAUNCHME   = $(FILESYSTEM)/LaunchMe
STACKSIZE  = 16384

ifeq ($(DEBUG),1)
OPT      = -O0
DEFFLAGS = -DDEBUG=1
else
OPT      = -O2
DEFFLAGS = -DNDEBUG=1
endif

INCPATH  = $(TDO_DEVKIT_PATH)/include
# -Isrc/openomf_shim MUST come before any path that would reach
# ../openomf-master/src so the slim shim headers win over the full openomf
# graph (see [[feedback-derive-dont-reimplement]] + port_3do/src/openomf_shim/).
INCFLAGS = -Isrc/openomf_shim -I$(INCPATH)/3do -I$(INCPATH)/community -I$(INCPATH)/ttl -Isrc

CFLAGS   = $(OPT) -bigend -za1 -zi4 -fa -fh -fx -fpu none -arch 3 -apcs "3/32/fp/swst/wide/softfp"
ASFLAGS  = -bigend -fpu none -arch 3 -apcs "3/32/fp/swst"

LIBPATH  = $(TDO_DEVKIT_PATH)/lib
LDFLAGS  = -match 0x1 -nodebug -noscanlib -nozeropad -verbose -remove -aif -reloc -dupok -ro-base 0
STARTUP  = $(LIBPATH)/3do/cstartup.o

LIBS =						\
	$(LIBPATH)/3do/3dlib.lib		\
	$(LIBPATH)/3do/audio.lib		\
	$(LIBPATH)/3do/clib.lib			\
	$(LIBPATH)/3do/codec.lib		\
	$(LIBPATH)/3do/compression.lib		\
	$(LIBPATH)/3do/DataAcq.lib		\
	$(LIBPATH)/3do/DS.lib			\
	$(LIBPATH)/3do/exampleslib.lib		\
	$(LIBPATH)/3do/filesystem.lib		\
	$(LIBPATH)/3do/graphics.lib		\
	$(LIBPATH)/3do/input.lib		\
	$(LIBPATH)/3do/international.lib	\
	$(LIBPATH)/3do/intmath.lib		\
	$(LIBPATH)/3do/lib3do.lib		\
	$(LIBPATH)/3do/music.lib		\
	$(LIBPATH)/3do/operamath.lib		\
	$(LIBPATH)/3do/string.lib		\
	$(LIBPATH)/3do/Subscriber.lib		\
	$(LIBPATH)/3do/swi.lib			\
	$(LIBPATH)/community/svc_funcs.lib      \
	$(LIBPATH)/community/svc_mem.lib

SRCS_C   = $(wildcard src/*.c)
SRCS_S   = $(wildcard src/*.s)

# openomf .c sources linked verbatim — list grows as we port more game
# systems (har.c, projectile.c, ...). See [[feedback-derive-dont-reimplement]].
#
# NOTE 2026-05-20: intersect.c is currently vendored under src/intersect.c
# with strictly mechanical C90 declaration hoisting (ARM SDT 2.51 doesn't
# accept mid-block declarations); top-of-file comment in src/intersect.c
# enumerates the diff. The OPENOMF_FW_SRCS list is empty for now but the
# build/openomf/%.o rule stays in place — first non-C99-tripping openomf
# .c we want will go through it.
OPENOMF_FW_SRCS =

OBJS  = $(SRCS_C:src/%.c=build/%.c.o)
OBJS += $(SRCS_S:src/%.s=build/%.s.o)
OBJS += $(OPENOMF_FW_SRCS:../openomf-master/src/%.c=build/openomf/%.o)
DEPS  = $(OBJS:.o=.d)

all: launchme modbin iso encrypt-iso

build/.touched:
	mkdir -p build && touch $@

builddir: build/.touched

objs: builddir $(OBJS)

$(LAUNCHME): objs
	armlink -o $@ $(LDFLAGS) $(STARTUP) $(LIBS) $(OBJS)

launchme: $(LAUNCHME)

modbin:
	modbin --name="$(NAME)" --time --stack=$(STACKSIZE) "$(LAUNCHME)" "$(LAUNCHME)"

iso/.touched:
	mkdir -p iso && touch $@

isodir: iso/.touched

iso: isodir
	3doiso -in "$(FILESYSTEM)" -out "$(ISONAME)"

encrypt-iso: $(ISONAME)
	3DOEncrypt genromtags "$(ISONAME)"

build/%.c.o: src/%.c
	armcc $(INCFLAGS) $(DEFFLAGS) $(CFLAGS) -M $< -o $@ > $(@:.o=.d)
	armcc $(INCFLAGS) $(DEFFLAGS) $(CFLAGS) -c $< -o $@

build/%.s.o: src/%.s
	armasm $(INCFLAGS) $(DEFFLAGS) $(ASFLAGS) $< -o $@

# openomf sources kept untouched (per [[feedback-isolated-project]]) — we
# pull them directly from ../openomf-master/ and resolve their includes
# against src/openomf_shim/ first.
build/openomf/%.o: ../openomf-master/src/%.c
	@mkdir -p $(dir $@)
	armcc $(INCFLAGS) $(DEFFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -rvf build iso $(LAUNCHME)

.PHONY: builddir isodir clean modbin iso encrypt-iso all launchme objs

-include $(DEPS)
