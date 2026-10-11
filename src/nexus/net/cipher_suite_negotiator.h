#pragma once
#include <string>
#include <vector>

namespace nexus::net {

class CipherSuiteNegotiator {
public:
    static std::string select_fastest_suite() {
        // Return AES-128-GCM as it is hardware-accelerated via AES-NI on hot paths
        return "TLS_AES_128_GCM_SHA256";
    }
};

} // namespace nexus::net
