#include "lan_bind.h"

#include <stddef.h>
#include <string.h>

bool lan_bind_valid(const char *s)
{
    if (!s || strnlen(s, LAN_BIND_LEN + 1) != LAN_BIND_LEN) return false;
    for (size_t i = 0; i < LAN_BIND_LEN; i++) {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool lan_bind_equal(const char *stored, const char *presented)
{
    // Length is not secret (it is fixed and public); content is. Reject a short/missing string before
    // reading 64 bytes of it, then compare all 64 with no early exit.
    if (!stored || !presented) return false;
    if (strnlen(stored, LAN_BIND_LEN + 1) != LAN_BIND_LEN || strnlen(presented, LAN_BIND_LEN + 1) != LAN_BIND_LEN)
        return false;
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < LAN_BIND_LEN; i++)
        diff |= (unsigned char)stored[i] ^ (unsigned char)presented[i];
    // Shape still matters: an all-zero-difference match of two garbage strings is not a token.
    return diff == 0 && lan_bind_valid(stored);
}
