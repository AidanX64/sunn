# GNU make ships a built-in CC=cc that ?= can never override; only apply the
# gcc default when the caller has not set CC themselves (env/command line win).
ifeq ($(origin CC),default)
CC := gcc
endif
CFLAGS ?= -Wall -Wextra -Werror -std=c2x -Iinclude
LDFLAGS ?=
LDLIBS ?=

TARGET_DIR := build

EXE :=
PREFIX ?= $(HOME)/.local/bin
define make_directory
mkdir -p "$(1)"
endef
define remove_directory
rm -rf "$(1)"
endef
define install_binary
cp "$(1)" "$(2)"
endef
define remove_file
rm -f "$(1)"
endef

ifeq ($(OS),Windows_NT)
EXE := .exe
PREFIX ?= $(USERPROFILE)/bin

# Select recipes by shell: MSYS2/Git Bash make runs under a POSIX shell
# (/bin/sh -> commands behave), while native Windows Make uses a bare
# sh.exe/cmd.exe with no Unix path and needs cmd /C.
ifeq (,$(findstring /,$(SHELL)))
define make_directory
cmd /C if not exist "$(1)" mkdir "$(1)"
endef
define remove_directory
cmd /C if exist "$(1)" rmdir /S /Q "$(1)"
endef
define install_binary
cmd /C copy /Y "$(1)" "$(2)" >NUL
endef
define remove_file
cmd /C if exist "$(1)" del /Q "$(1)"
endef
endif
endif

BIN := $(TARGET_DIR)/forge$(EXE)
SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:src/%.c=$(TARGET_DIR)/%.o)
DEPFILES := $(OBJECTS:.o=.d)

.PHONY: all clean test install uninstall regression

all: $(BIN)

$(TARGET_DIR):
	$(call make_directory,$(TARGET_DIR))

# Header tracking, the same -MMD approach forge uses for the projects it
# builds: editing a header recompiles exactly the objects that include it.
# (Without this, struct changes in include/ silently kept stale objects.)
$(TARGET_DIR)/%.o: src/%.c | $(TARGET_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

-include $(DEPFILES)

$(BIN): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@

# Installs the forge binary to a user-level bin directory (no sudo required).
install: $(BIN)
	$(call make_directory,$(PREFIX))
	$(call install_binary,$(BIN),$(PREFIX)/forge$(EXE))
	@echo "Installed forge to $(PREFIX)/forge$(EXE)"
	@echo "Add $(PREFIX) to your PATH to call 'forge' from anywhere."

uninstall:
	$(call remove_file,$(PREFIX)/forge$(EXE))

clean:
	$(call remove_directory,$(TARGET_DIR))

test: all
	$(BIN) --help

# The regression suites are bash scripts. Native-Windows make drives recipes
# through cmd.exe, where bare "bash" resolves to the WSL launcher and dies
# without a distro installed; probe the common Git-for-Windows/MSYS2 spots
# first (space-free short paths so $(wildcard) can match them).
ifeq ($(OS),Windows_NT)
GIT_BASH := $(firstword $(wildcard C:/Progra~1/Git/bin/bash.exe) \
                        $(wildcard C:/msys64/usr/bin/bash.exe))
BASH := $(if $(GIT_BASH),$(GIT_BASH),bash)
else
BASH := bash
endif

# Sandbox dependency/engine regression suites (see test/*.sh headers).
regression: all
	"$(BASH)" test/deps-regression.sh
	"$(BASH)" test/registry-regression.sh
	"$(BASH)" test/regression.sh
