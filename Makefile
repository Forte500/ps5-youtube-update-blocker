# youtube-update-blocker
#
# Build:  make
# Clean:  make clean
#
# The PS5 payload SDK is required. If it is not in ~/ps5-payload-sdk:
#   make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk

PS5_PAYLOAD_SDK ?= $(HOME)/ps5-payload-sdk

BIN := youtube-update-blocker.elf
SRC := src/main.c src/sqlite_patch.c src/sha256.c src/notify.c
HDR := $(wildcard src/*.h)

.PHONY: all clean

all: $(BIN)

ifeq ($(wildcard $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk),)
$(error PS5_PAYLOAD_SDK not found at "$(PS5_PAYLOAD_SDK)". Install the SDK or run: make PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk)
endif

include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

PS5FLAGS := -std=c11 -Wall -Wextra -O2 -g

$(BIN): $(SRC) $(HDR)
	$(CC) $(PS5FLAGS) -o $@ $(SRC) -lSceNotification
	@echo ""
	@echo "Built $@"

clean:
	rm -f $(BIN)
