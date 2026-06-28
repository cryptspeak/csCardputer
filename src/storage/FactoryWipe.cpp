#include "FactoryWipe.h"
#include "FlashStore.h"
#include "SDStore.h"

#include <Arduino.h>
#include <Preferences.h>

namespace FactoryWipe {

void wipeAll(FlashStore* flash, SDStore* sd) {
    // 1. Clear every NVS namespace this firmware writes to.
    {
        Preferences prefs;
        if (prefs.begin("ratcom", false))        { prefs.clear(); prefs.end(); }
        if (prefs.begin("ratcom_cfg", false))     { prefs.clear(); prefs.end(); }
        if (prefs.begin("ratcom_id", false))      { prefs.clear(); prefs.end(); }
        if (prefs.begin("ratcom_duress", false))  { prefs.clear(); prefs.end(); }
        Serial.println("[WIPE] NVS cleared");
    }

    // 2. Wipe the SD card's /ratcom data tree.
    if (sd && sd->isReady()) {
        sd->wipeStandalone();
        Serial.println("[WIPE] SD wiped");
    }

    // 3. Format the flash LittleFS partition.
    if (flash) {
        flash->format();
        Serial.println("[WIPE] Flash formatted");
    }
}

}  // namespace FactoryWipe
