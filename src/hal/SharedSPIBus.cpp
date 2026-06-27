#include "SharedSPIBus.h"

static SemaphoreHandle_t sharedSPIMutexHandle() {
    static SemaphoreHandle_t sharedSPIMutex = xSemaphoreCreateRecursiveMutex();
    static bool warned = false;
    if (!sharedSPIMutex && !warned) {
        warned = true;
        // Without this mutex, every SharedSPILock silently fails to lock (locked()
        // returns false) and all radio/SD SPI access goes dark with no other signal.
        Serial.println("[SPI_BUS] Failed to create shared SPI mutex — radio and SD access disabled");
    }
    return sharedSPIMutex;
}

SharedSPILock::SharedSPILock(TickType_t timeout) {
    SemaphoreHandle_t mutex = sharedSPIMutexHandle();
    _locked = mutex && (xSemaphoreTakeRecursive(mutex, timeout) == pdTRUE);
}

SharedSPILock::~SharedSPILock() {
    if (_locked) {
        xSemaphoreGiveRecursive(sharedSPIMutexHandle());
    }
}
