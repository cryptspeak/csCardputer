#include "ContactsEncryption.h"

namespace ContactsEncryption {

const AtRestCrypto::Domain kDomain = {
    {'R', 'C', 'N', '1'},
    "rscardputer.contacts.v1",
};

}  // namespace ContactsEncryption
