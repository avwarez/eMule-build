// eMule TF-PSA-Crypto user config override.
//
// mbedTLS 4.0 cross-checks the legacy config (mbedtls_config.h /
// MBEDTLS_USER_CONFIG_FILE) against the PSA crypto config (psa/crypto_config.h /
// TF_PSA_CRYPTO_USER_CONFIG_FILE) and errors out if a threading option is set
// on only one side. See emule_mbedtls_user_config.h for the full explanation
// of why MBEDTLS_THREADING_ALT is needed at all.
#pragma once

#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_ALT
