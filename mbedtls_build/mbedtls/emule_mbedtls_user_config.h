// eMule mbedTLS user config override.
//
// Included automatically after the vendored mbedtls_config.h via the
// MBEDTLS_USER_CONFIG_FILE mechanism (see mbedtls/include/mbedtls/mbedtls_config.h) -
// the vanilla mbedtls/tf-psa-crypto submodules are never edited directly.
//
// Enables the MBEDTLS_THREADING_ALT extension point: TLSthreading.cpp/.h
// implement mbedtls's mutex/condition-variable callbacks directly on top of
// Win32 CRITICAL_SECTION, with the matching type definitions supplied by
// threading_alt.h in this same directory (found via the same #include
// "threading_alt.h" / #include "emule_mbedtls_user_config.h" search-path
// mechanism as this file).
#pragma once

#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT
