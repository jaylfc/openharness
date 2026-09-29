// The bind token: what lets a computer that has once been plugged into this dial reach it again over
// the LAN. The pure half of that rule — the shape of a token and the comparison — kept apart from the
// storage and the sockets so it can be proved on a host with a plain compiler (test/test_lan_bind.c).
//
// The daemon mints 32 random bytes, sends them as 64 lowercase hex characters in `welcome.bind` over the
// cable, and later presents the same string on a TCP session. The dial keeps what the cable told it.
#pragma once

#include <stdbool.h>

#define LAN_BIND_LEN 64   // characters, not counting the NUL

// Exactly LAN_BIND_LEN characters of [0-9a-f]. Anything else — wrong length, upper case, NUL early — is
// not a token, and is never stored or accepted.
bool lan_bind_valid(const char *s);

// True only when both are valid tokens and equal. The comparison always walks all LAN_BIND_LEN bytes and
// never branches on where the first difference is: a LAN peer can time this, and the stored token is the
// only thing standing between it and the dial. `presented` is the untrusted side; a NULL or malformed one
// is simply false.
bool lan_bind_equal(const char *stored, const char *presented);
