#pragma once

#include <cstddef>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cstring>
#endif

namespace schwab {

/**
 * @brief Cryptographically secure zero-memory function preventing compiler dead-store elimination.
 *
 * @param ptr Pointer to memory buffer to wipe.
 * @param size Number of bytes to scrub.
 */
inline void secure_zero_memory(void* ptr, size_t size) noexcept {
    if (ptr == nullptr || size == 0) {
        return;
    }
#if defined(_WIN32)
    SecureZeroMemory(ptr, size);
#elif defined(__STDC_LIB_EXT1__) || defined(HAVE_EXPLICIT_BZERO)
    explicit_bzero(ptr, size);
#else
    volatile unsigned char* p = static_cast<volatile unsigned char*>(ptr);
    while (size--) {
        *p++ = 0;
    }
#endif
}

/**
 * @brief Securely scrubs a std::string before resetting its capacity.
 *
 * Ensures cryptographic credentials, tokens, and account identifiers
 * do not persist in heap memory after use.
 *
 * @param str Reference to string to wipe and clear.
 */
inline void secure_clear(std::string& str) noexcept {
    if (!str.empty()) {
        secure_zero_memory(str.data(), str.size());
        str.clear();
    }
}

} // namespace schwab
