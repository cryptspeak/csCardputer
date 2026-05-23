#pragma once

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

class SDStore {
public:
    bool begin(SPIClass* spi, int csPin);
    void end();

    // Atomic write: .tmp -> verify -> .bak -> rename
    bool writeAtomic(const char* path, const uint8_t* data, size_t len);
    bool writeString(const char* path, const String& data);

    // Simple direct write (no rename dance — for small config files)
    bool writeDirect(const char* path, const uint8_t* data, size_t len);
    String readString(const char* path);

    bool ensureDir(const char* path);
    bool exists(const char* path);
    bool remove(const char* path);
    File openDir(const char* path);
    bool removeDir(const char* path);
    bool readFile(const char* path, uint8_t* buffer, size_t maxLen, size_t& bytesRead);

    bool isReady() const { return _ready; }
    uint64_t totalBytes() const;
    uint64_t usedBytes() const;

    // Creates the legacy /ratcom/ directory tree (does NOT reformat FAT)
    bool formatForStandalone();

    // Recursively delete the legacy /ratcom/* data and recreate clean dirs
    bool wipeStandalone();

    // Check if SD has data from a previous install
    bool hasExistingData();

private:
    void wipeDir(const char* path);
    bool _ready = false;
};
