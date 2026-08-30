#include "tls_bundle.h"

// Symbols exported by the x509_crt_bundle object linked into libmbedtls.a.
extern const uint8_t x509_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t x509_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

void useRootCaBundle(WiFiClientSecure& client) {
    client.setCACertBundle(x509_crt_bundle_start,
                           (size_t)(x509_crt_bundle_end - x509_crt_bundle_start));
}
