# mbedtls_build/

`eMule/mbedtls` (and its nested `tf-psa-crypto`/`framework` submodules) is a
vanilla, unmodified checkout of upstream mbedTLS - never edited in place.
eMule needs two customizations on top of it; both are supplied from outside
the submodule, using extension points mbedTLS itself documents for this
purpose, so there is nothing to re-apply after updating the submodule to a
newer tag.

## `mbedtls/emule_mbedtls_user_config.h`

Defines `MBEDTLS_THREADING_ALT`. Wired in via the `MBEDTLS_USER_CONFIG_FILE`
CMake cache variable (set in the top-level `CMakeLists.txt` before
`add_subdirectory(mbedtls)`), which mbedtls's own build turns into a
`MBEDTLS_USER_CONFIG_FILE="<path>"` compile definition - `PUBLIC` on the
`MbedTLS::mbedtls` target, so `srchybrid` gets it too just by linking against
that target, no separate wiring needed there.

## `mbedtls/threading_alt.h`

eMule's own file (not from mbedTLS), authored by the eMule devs: implements
`mbedtls_platform_mutex_t` / `mbedtls_platform_condition_variable_t` on top
of Win32 `CRITICAL_SECTION`. Consumed two ways:

- `mbedtls/tf-psa-crypto/drivers/builtin/src/threading.c` (inside the
  submodule) does `#include "threading_alt.h"` when `MBEDTLS_THREADING_ALT`
  is defined.
- `srchybrid/TLSthreading.h` does the same, directly.

Both are *unqualified* quoted includes (`#include "threading_alt.h"`, no
`mbedtls/` prefix in the directive itself), so the compiler first checks the
including file's own directory, then falls through to the `/I` search path.
mbedTLS deliberately ships no `threading_alt.h` in its real include tree (it
only has dummies under `tests/include/alt-dummy/`, which aren't on the
build's include path), so that first check always misses and the compiler
picks up this directory instead - added via a single `include_directories()`
call in the top-level `CMakeLists.txt`, ahead of both `add_subdirectory(mbedtls)`
and `add_subdirectory(srchybrid)`, so it reaches every translation unit that
needs it without hardcoding mbedtls's internal CMake target names.

## Why the directory is called `mbedtls/`

Not required by the mechanism above (both includes are unqualified, so a
flat directory would resolve too) - kept for readability, so the override
tree's shape mirrors the header it's standing in for.
