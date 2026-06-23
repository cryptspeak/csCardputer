#include "SettingsEncryption.h"

namespace SettingsEncryption {

const AtRestCrypto::Domain kDomain = {
    {'R', 'U', 'C', '1'},
    "rscardputer.settings.v1",
};

}  // namespace SettingsEncryption
