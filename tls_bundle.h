#pragma once
#include <WiFiClientSecure.h>

// ============================================================
// Root-CA bundle for arbitrary public HTTPS hosts.
//
// The Arduino core's mbedtls is built with
// CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_DEFAULT_FULL, which embeds the full
// Mozilla root store (~140 CAs) inside libmbedtls.a. Pointing a client at
// it gives real certificate validation for domains we can't pin ahead of
// time — a merchant's Lightning Address host, its LNURL callback/verify
// host, and the public rate APIs — instead of setInsecure().
// ============================================================
void useRootCaBundle(WiFiClientSecure& client);
