// Compatibility shim for stdext::make_checked_array_iterator/
// make_unchecked_array_iterator - non-standard MSVC STL (Dinkumware)
// extensions that cryptopp/integer.cpp and cryptopp/zdeflate.cpp call
// unconditionally on any MSVC version >= 1500/1600 (see CRYPTOPP_MSC_VERSION
// checks there), assuming every such MSVC ships them. Newer MSVC STL
// releases have removed them entirely (they were always just a way to wrap a
// raw pointer for the old "Checked Iterators"/SCL debug feature, never real
// bounds enforcement cryptopp's logic depends on) - a plain pointer is a
// fully conforming RandomAccessIterator and works as a drop-in replacement
// for every call site that uses these two functions.
//
// Only force-included (see cryptopp_build/CMakeLists.txt) when a configure-time
// check confirms the real stdext:: versions aren't available, so this has no
// effect - and doesn't conflict/collide with the genuine ones - on a toolset
// that still ships them.
#ifndef EMULE_CRYPTOPP_MSVC_STDEXT_COMPAT_H
#define EMULE_CRYPTOPP_MSVC_STDEXT_COMPAT_H

#include <cstddef>

namespace stdext {

template <typename T>
inline T* make_checked_array_iterator(T* first, std::size_t /*size*/, std::size_t /*index*/ = 0)
{
    return first;
}

template <typename T>
inline T* make_unchecked_array_iterator(T* first)
{
    return first;
}

} // namespace stdext

#endif // EMULE_CRYPTOPP_MSVC_STDEXT_COMPAT_H
