THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))

PROJECT  := $(lastword $(subst /, ,$(CURDIR)))
SRC_DIR  := src
INC_DIR  := include
BUILD    := build
CONFIGS  := debug release

ifeq ($(OS),Windows_NT)
  EXE      := .exe
  PLATFORM := Windows
  ifdef MSYSTEM                
    POSIX_SHELL := 1
  else                          
    SHELL := cmd.exe
  endif
else
  EXE         :=
  POSIX_SHELL := 1
  PLATFORM    := $(shell uname -s)
endif

TARGET := $(PROJECT)$(EXE)

ifdef POSIX_SHELL
  FIX   = $1
  MKDIR = mkdir -p "$1"
  RMDIR = rm -rf "$1"
  RUN   = "./$1" $(ARGS)
else
  FIX   = $(subst /,\,$1)
  MKDIR = if not exist "$(call FIX,$1)" mkdir "$(call FIX,$1)"
  RMDIR = if exist "$(call FIX,$1)" rmdir /S /Q "$(call FIX,$1)"
  RUN   = "$(call FIX,$1)" $(ARGS)
endif

.SUFFIXES:           
%.o: %.c             
%: %.c
%: %.o
.SECONDARY:          
.DELETE_ON_ERROR:     

rwildcard = $(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))

SRCS     := $(call rwildcard,$(SRC_DIR),*.c)
INC_DIRS := $(sort $(INC_DIR) $(patsubst %/,%,$(dir $(call rwildcard,$(INC_DIR),*.h))))

ifeq ($(origin CC),default)
  CC := gcc
endif
IS_CLANG := $(findstring clang,$(shell $(CC) --version))

CPPFLAGS := $(addprefix -I,$(INC_DIRS)) -I$(SRC_DIR) -MMD -MP
CFLAGS   := -std=c2x -Wall -Wextra -Wpedantic
LDFLAGS  :=
LDLIBS   := -lm

debug_CPPFLAGS :=
debug_CFLAGS   := -g3 -O0
debug_LDFLAGS  :=

release_CPPFLAGS := -DNDEBUG
release_CFLAGS   := -O3 -march=native
release_LDFLAGS  :=
ifeq ($(IS_CLANG),)
  release_CFLAGS  += -flto=auto
  release_LDFLAGS += -flto=auto
endif

define CONFIG_RULES
$(1)_DIR  := $(BUILD)/$(1)
$(1)_OBJ  := $$($(1)_DIR)/obj
$(1)_BIN  := $$($(1)_DIR)/$(TARGET)
$(1)_OBJS := $$(patsubst %.c,$$($(1)_OBJ)/%.o,$(SRCS))

$$($(1)_BIN): $$($(1)_OBJS)
	@$$(call MKDIR,$$(@D))
	$$(CC) $$(CFLAGS) $$($(1)_CFLAGS) $$^ -o $$@ $$(LDFLAGS) $$($(1)_LDFLAGS) $$(LDLIBS)

$$($(1)_OBJ)/%.i: %.c $(THIS_MAKEFILE)
	@$$(call MKDIR,$$(@D))
	$$(CC) $$(CPPFLAGS) $$($(1)_CPPFLAGS) $$(CFLAGS) $$($(1)_CFLAGS) -E $$< -o $$@ -MT $$@ -MF $$(@:.i=.d)

$$($(1)_OBJ)/%.s: $$($(1)_OBJ)/%.i
	$$(CC) $$(CFLAGS) $$($(1)_CFLAGS) -S $$< -o $$@

$$($(1)_OBJ)/%.o: $$($(1)_OBJ)/%.s
	$$(CC) $$(CFLAGS) $$($(1)_CFLAGS) -c $$< -o $$@

-include $$($(1)_OBJS:.o=.d)
endef

$(foreach c,$(CONFIGS),$(eval $(call CONFIG_RULES,$c)))

.PHONY: build-debug build-release run-debug run-release clean
.DEFAULT_GOAL := build-debug

build-debug:   $(debug_BIN)
build-release: $(release_BIN)

run-debug: build-debug
	$(call RUN,$(debug_BIN))

run-release: build-release
	$(call RUN,$(release_BIN))

clean:
	$(call RMDIR,$(BUILD))