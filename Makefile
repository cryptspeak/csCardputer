STANDALONE_ENV         ?= standalone_915
BUILD_DIR              ?= build
DIST_DIR               ?= dist

STANDALONE_NAME        := rscardputer-standalone
STANDALONE_BIN         := $(BUILD_DIR)/$(STANDALONE_NAME).bin
STANDALONE_APP_BIN     := .pio/build/$(STANDALONE_ENV)/firmware.bin
STANDALONE_FACTORY_BIN := rscardputer-standalone-factory.bin

PORT ?= $(port)
ifeq ($(PORT),)
PORT := /dev/ttyACM0
endif

.PHONY: all build build-standalone image package release flash clean

all: package

build: build-standalone

build-standalone:
	pio run -e $(STANDALONE_ENV)

image: build-standalone
	mkdir -p $(BUILD_DIR)
	cp $(STANDALONE_FACTORY_BIN) $(STANDALONE_BIN)

package: image
	mkdir -p $(DIST_DIR)
	python3 tools/package_merged_zip.py \
		--image $(STANDALONE_BIN) \
		--name $(STANDALONE_NAME) \
		--output $(DIST_DIR)/$(STANDALONE_NAME).zip

release: package

flash: image
	esptool --chip esp32s3 --port $(PORT) --baud 460800 \
		--before default_reset --after hard_reset write-flash 0x0 $(STANDALONE_BIN)

clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR) $(STANDALONE_FACTORY_BIN)
