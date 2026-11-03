/* Ed25519 signature verification (RFC 8032) for the launcher's signed updates. See ed25519.c. */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* 1 = sig is a valid signature of msg by pub (64-byte signature R || S, 32-byte public key). */
int ed25519_verify(const uint8_t sig[64], const void* msg, size_t len, const uint8_t pub[32]);

/* SHA-512 of a || b || c (Windows CNG); 1 = ok */
int ed25519_sha512(const void* a, size_t na, const void* b, size_t nb, const void* c, size_t nc, uint8_t out[64]);

/* n bytes from 2n hex digits; 1 = ok */
int ed25519_from_hex(const char* hex, uint8_t* out, size_t n);
