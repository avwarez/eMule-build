// eMule mbedTLS user config override.
//
// Included automatically after the vendored mbedtls_config.h via the
// MBEDTLS_USER_CONFIG_FILE mechanism (see mbedtls/include/mbedtls/mbedtls_config.h) -
// the vanilla mbedtls/tf-psa-crypto submodules are never edited directly.
//
// Enables the MBEDTLS_THREADING_ALT extension point: TLSthreading.cpp/.h
// implement mbedtls's mutex/condition-variable callbacks directly on top of
// Win32 CRITICAL_SECTION, with the matching type definitions supplied by
// threading_alt.h - vendored inside the emule submodule itself (see the
// include_directories() call in the top-level CMakeLists.txt), not in this
// directory.
#pragma once

#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT
