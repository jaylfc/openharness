// The bind token's shape and comparison (main/lan_bind.c).
#include "lan_bind.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static const char *A = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";

int main(void)
{
    // Shape: exactly 64 lowercase hex characters.
    assert(lan_bind_valid(A));
    assert(!lan_bind_valid(NULL) && !lan_bind_valid("") && !lan_bind_valid("abc"));
    char t[LAN_BIND_LEN + 8];
    memcpy(t, A, LAN_BIND_LEN + 1);
    t[LAN_BIND_LEN] = 'a'; t[LAN_BIND_LEN + 1] = 0;
    assert(!lan_bind_valid(t));                                   // 65 characters
    memcpy(t, A, LAN_BIND_LEN + 1); t[LAN_BIND_LEN - 1] = 0;
    assert(!lan_bind_valid(t));                                   // 63 characters
    for (int i = 0; i < LAN_BIND_LEN; i++) {                      // every position rejects every non-hex
        memcpy(t, A, LAN_BIND_LEN + 1);
        static const char bad[] = "ABCDEFgG -+/\n\t:@`";
        for (const char *c = bad; *c; c++) { t[i] = *c; assert(!lan_bind_valid(t)); }
        t[i] = 0; assert(!lan_bind_valid(t));                     // NUL early
    }
    for (int c = 0; c < 256; c++) {                               // exactly 0-9a-f are accepted
        memcpy(t, A, LAN_BIND_LEN + 1); t[10] = (char)c;
        const int hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        assert(lan_bind_valid(t) == (hex != 0 && c != 0));
    }

    // Equality: identical valid tokens only.
    assert(lan_bind_equal(A, A));
    char b[LAN_BIND_LEN + 1];
    memcpy(b, A, sizeof b);
    assert(lan_bind_equal(A, b));
    for (int i = 0; i < LAN_BIND_LEN; i++) {                      // a difference anywhere, first or last
        memcpy(b, A, sizeof b); b[i] = A[i] == 'f' ? '0' : (char)(A[i] == '9' ? 'a' : A[i] + 1);
        assert(!lan_bind_equal(A, b) && !lan_bind_equal(b, A));
    }
    assert(!lan_bind_equal(NULL, A) && !lan_bind_equal(A, NULL) && !lan_bind_equal(NULL, NULL));
    assert(!lan_bind_equal("", ""));                              // nothing stored never matches nothing presented
    assert(!lan_bind_equal(A, ""));
    assert(!lan_bind_equal(A, "0123456789abcdef"));
    memcpy(t, A, LAN_BIND_LEN + 1); t[LAN_BIND_LEN] = '0'; t[LAN_BIND_LEN + 1] = 0;
    assert(!lan_bind_equal(A, t) && !lan_bind_equal(t, A));       // longer, same prefix
    const char *junk = "................................................................";
    assert(!lan_bind_equal(junk, junk));                          // equal garbage is still not a token

    puts("lan_bind: token shape, every-position mutations, NULL/short/long/garbage PASS");
    return 0;
}
