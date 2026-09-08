# Makefile build
# meant to be extremely portable to weird unix-like systems

CC := cc
PKG_CONFIG := pkg-config

empty :=
space := $(empty) $(empty)

ifeq ($(filter clean distclean,$(MAKECMDGOALS)),)

-include compat/config.mk

ifndef DISABLE_MMD
DEPFLAGS = -MMD -MP -MF $(@:.$(OBJ_EXT)=.d)
endif

# trigger configure re-run if $(CC) changes
_dummy := $(shell \
	printf '$(CC)' > compat/tmp/cc-new; \
	cmp -s compat/tmp/cc-new compat/tmp/cc || \
	{ rm -f compat/tmp/cc; mv compat/tmp/cc-new compat/tmp/cc; }; \
	rm -f compat/tmp/cc-new \
)

endif

ifndef DISABLE_VM_GML_PROFILER
DEFINES += $(DEFINE)ENABLE_VM_GML_PROFILER
endif
ifndef DISABLE_VM_OPCODE_PROFILER
DEFINES += $(DEFINE)ENABLE_VM_OPCODE_PROFILER
endif
ifndef DISABLE_VM_STUB_LOGS
DEFINES += $(DEFINE)ENABLE_VM_STUB_LOGS
endif
ifndef DISABLE_VM_TRACING
DEFINES += $(DEFINE)ENABLE_VM_TRACING
endif

INCLUDES += $(INC). \
		    $(INC)src \
		    $(INC)vendor/stb/ds \
		    $(INC)src/image \
		    $(INC)vendor/stb/image \
		    $(INC)vendor/stb/vorbis \
		    $(INC)vendor/md5 \
		    $(INC)vendor/sha1 \
		    $(INC)vendor/base64 \
		    $(INC)vendor/bzip2

HEADERS += $(wildcard src/*.h) $(shell find vendor -name '*.h')
SRCS += $(wildcard src/*.c) $(wildcard src/image/*.c) $(wildcard vendor/bzip2/*.c) vendor/md5/md5.c vendor/sha1/sha1.c vendor/base64/base64.c

PLATFORM := cli
BACKEND := glfw3
AUDIO_BACKEND := miniaudio

ifdef BUTTERSCOTCH_COMMIT_DATE
DEFINES += $(DEFINE)BUTTERSCOTCH_COMMIT_DATE=\"$(BUTTERSCOTCH_COMMIT_DATE)\"
else
DEFINES += $(DEFINE)BUTTERSCOTCH_COMMIT_DATE=\"unknown\"
endif
ifdef BUTTERSCOTCH_COMMIT_HASH
DEFINES += $(DEFINE)BUTTERSCOTCH_COMMIT_HASH=\"$(BUTTERSCOTCH_COMMIT_HASH)\"
else
DEFINES += $(DEFINE)BUTTERSCOTCH_COMMIT_HASH=\"unknown\"
endif

ifndef DISABLE_WAD14
DEFINES += $(DEFINE)ENABLE_WAD14
endif

ifndef DISABLE_WAD16
DEFINES += $(DEFINE)ENABLE_WAD16
endif

ifndef DISABLE_WAD17
DEFINES += $(DEFINE)ENABLE_WAD17
endif

SRCS += $(wildcard src/$(PLATFORM)/*.c)
SRCS += $(wildcard src/backends/$(BACKEND).*)
INCLUDES += $(INC)src/$(PLATFORM)
ifeq ($(OS),Windows)
PKG_CONFIG_FLAGS := --static
endif
ifeq ($(BACKEND),glfw3)
GLFW3_CFLAGS := $(shell $(PKG_CONFIG) $(PKG_CONFIG_FLAGS) --cflags glfw3)
GLFW3_LIBS := $(shell $(PKG_CONFIG) $(PKG_CONFIG_FLAGS) --libs glfw3)
SYSCFLAGS += $(GLFW3_CFLAGS)
LIBS += $(GLFW3_LIBS)
DEFINES += $(DEFINE)USE_GLFW3
ENABLE_GLAD := 1
ifdef ENABLE_GLES
DISABLE_SW_RENDERER := 1
endif
endif
ifeq ($(BACKEND),glfw2)
GLFW2_CFLAGS := $(shell $(PKG_CONFIG) $(PKG_CONFIG_FLAGS) --cflags libglfw)
GLFW2_LIBS += $(shell $(PKG_CONFIG) $(PKG_CONFIG_FLAGS) --libs libglfw)
SYSCFLAGS += $(GLFW2_CFLAGS)
LIBS += $(GLFW2_LIBS)
DEFINES += $(DEFINE)USE_GLFW2
ENABLE_GLAD := 1
ifdef ENABLE_GLES
DISABLE_SW_RENDERER := 1
endif
endif
ifeq ($(BACKEND),sdl1)
SDL1_CFLAGS := $(shell $(PKG_CONFIG) $(PKG_CONFIG_FLAGS) --cflags sdl)
SDL1_LIBS += $(shell $(PKG_CONFIG) $(PKG_CONFIG_FLAGS) --libs sdl)
SYSCFLAGS += $(SDL1_CFLAGS)
LIBS += $(SDL1_LIBS)
DEFINES += $(DEFINE)USE_SDL1
endif
ifeq ($(BACKEND),sdl2)
SDL2_CFLAGS := $(shell $(PKG_CONFIG) $(PKG_CONFIG_FLAGS) --cflags sdl2)
SDL2_LIBS += $(shell $(PKG_CONFIG) $(PKG_CONFIG_FLAGS) --libs sdl2)
SYSCFLAGS += $(SDL2_CFLAGS)
LIBS += $(SDL2_LIBS)
DEFINES += $(DEFINE)USE_SDL2
endif
ifeq ($(BACKEND),sdl3)
SDL3_CFLAGS := $(shell $(PKG_CONFIG) $(PKG_CONFIG_FLAGS) --cflags sdl3)
SDL3_LIBS += $(shell $(PKG_CONFIG) $(PKG_CONFIG_FLAGS) --libs sdl3)
SYSCFLAGS += $(SDL3_CFLAGS)
LIBS += $(SDL3_LIBS)
DEFINES += $(DEFINE)USE_SDL3
endif
ifeq ($(BACKEND),appkit)
LIBS += -framework Cocoa -framework GameController
DEFINES += $(DEFINE)USE_APPKIT
SYSCFLAGS += -Wno-deprecated-declarations
endif
ifeq ($(BACKEND),noop)
DISABLE_LEGACY_GL := 1
DISABLE_MODERN_GL := 1
DEFINES += $(DEFINE)USE_NOOP
endif

# Noop renderer is exclusive to noop backend; GL renderers exclusive to non-noop backends
ifneq ($(BACKEND),noop)
# GNU make doesn't have a way to do OR in conditionals, stupid language for clowns
ifndef DISABLE_LEGACY_GL
ENABLE_GL := 1
endif
ifndef DISABLE_MODERN_GL
ENABLE_GL := 1
endif

ifdef ENABLE_GL
SRCS += $(wildcard src/gl_common/*.c)
INCLUDES += $(INC)src/gl_common $(INC)src/gl
HEADERS += $(wildcard src/gl_common/*.h)
ENABLE_GLAD := 1
endif

ifndef DISABLE_LEGACY_GL
DEFINES += $(DEFINE)ENABLE_LEGACY_GL
SRCS += $(wildcard src/gl_legacy/*.c)
INCLUDES += $(INC)src/gl_legacy
HEADERS += $(wildcard src/gl_legacy/*.h) $(wildcard src/gl/*.h)
endif

ifndef DISABLE_MODERN_GL
DEFINES += $(DEFINE)ENABLE_MODERN_GL
SRCS += $(wildcard src/gl/*.c)
HEADERS += $(wildcard src/gl/*.h)
endif
endif

ifeq ($(BACKEND),noop)
ifndef DISABLE_NOOP_RENDERER
DEFINES += $(DEFINE)ENABLE_NOOP_RENDERER
endif
endif

ifndef DISABLE_SW_RENDERER
DEFINES += -DENABLE_SW_RENDERER
SRCS += $(wildcard src/sw/*.c)
HEADERS += $(wildcard src/sw/*.h)
INCLUDES += -Isrc/sw
endif

ifdef DISABLE_WAD14
ifdef DISABLE_WAD16
ifdef DISABLE_WAD17
$(error must enable at least 1 bytecode version)
endif
endif
endif

ifeq ($(BACKEND),noop)
ifdef DISABLE_NOOP_RENDERER
$(error must enable at least 1 renderer)
endif
else
ifdef DISABLE_LEGACY_GL
ifdef DISABLE_MODERN_GL
ifdef DISABLE_SW_RENDERER
$(error must enable at least 1 renderer)
endif
endif
endif

ifeq ($(AUDIO_BACKEND),miniaudio)
INCLUDES += $(INC)src/audio/miniaudio $(INC)vendor/miniaudio
DEFINES += $(DEFINE)USE_MINIAUDIO
SRCS += $(wildcard src/audio/miniaudio/*.c)
HEADERS += $(wildcard src/audio/miniaudio/*.h)
ifneq ($(OS),Windows)
LIBS += -pthread
endif
endif
ifeq ($(AUDIO_BACKEND),openal)
INCLUDES += $(INC)src/audio/openal
DEFINES += $(DEFINE)USE_OPENAL
SRCS += $(wildcard src/audio/openal/*.c)
HEADERS += $(wildcard src/audio/openal/*.h)
ifeq ($(OS),Darwin)
LIBS += -framework OpenAL
else
LIBS += -lopenal
endif
endif

ifdef ENABLE_GLAD
SRCS += vendor/glad/src/glad.c
INCLUDES += $(INC)vendor/glad/include
endif

ifeq ($(OS),Windows)
ifeq ($(SYNTAX),msvc)
LIBS += winmm.lib
DEFINES += $(DEFINE)_CRT_SECURE_NO_WARNINGS $(DEFINE)_CRT_SECURE_NO_DEPRECATE
else
LIBS += -static
LIBS += -lwinmm
endif
DEFINES += $(DEFINE)WIN32_LEAN_AND_MEAN
else
ifeq ($(OS),Darwin)
LIBS += -lobjc
else
LIBS += -lm
endif
endif

ifndef VERBOSE
V := @
endif

OBJS := $(addprefix build/,$(SRCS))
OBJS := $(OBJS:%=%.$(OBJ_EXT))

all: build/butterscotch

-include $(OBJS:.$(OBJ_EXT)=.d)

ifeq ($(filter clean distclean,$(MAKECMDGOALS)),)

compat/config.mk: compat/configure.sh compat/tmp/cc
	@CC="$(CC)" $(SHELL) compat/configure.sh

endif

build/butterscotch: $(OBJS)
	@{ [ -z "$(NO_COLOR)" ] && [ -t 1 ]; } && printf " \033[1;34mLD\033[0m butterscotch\n" || printf " LD butterscotch\n"
	$(V)MSYS2_ARG_CONV_EXCL='*' $(_CC) $(LDFLAGS) $(OBJS) $(LIBS) $(EXTRALIBS) $(OUTPUT_EXE)$@
	@[ -f $@.exe ] && chmod +x $@.exe || true

build/%.$(OBJ_EXT): % compat/config.mk $(if $(DISABLE_MMD),$(HEADERS))
	@mkdir -p $(dir $@)
	@{ [ -z "$(NO_COLOR)" ] && [ -t 1 ]; } && printf " \033[1;32mCC\033[0m $<\n" || printf " CC $<\n"
	$(V)MSYS2_ARG_CONV_EXCL='*' $(_CC) $(DEFINES) $(INCLUDES) $(SYSCFLAGS) $(CFLAGS) $(DEPFLAGS) $(COMPILE_OBJ) $(SRCFLAG)$< $(OUTPUT_OBJ)$@

clean:
	rm -rf build

distclean: clean
	rm -f compat/config.mk compat/tmp/cc
