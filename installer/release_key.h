/* The public half of the release signing key (scripts/release_keygen.py). The launcher accepts an update
 * only if its manifest is signed by the matching private key, which never leaves the release machine.
 * Replacing this key means older launchers cannot verify newer releases. */
#pragma once
#define SLY2_RELEASE_PUBKEY_HEX "2d2fa1daea71ec92d9dc78c064a0e39379c2ac8ebb8628a875a569a3f8a9bde4"
static const unsigned char SLY2_RELEASE_PUBKEY[32] = { 0x2d, 0x2f, 0xa1, 0xda, 0xea, 0x71, 0xec, 0x92, 0xd9, 0xdc, 0x78, 0xc0, 0x64, 0xa0, 0xe3, 0x93, 0x79, 0xc2, 0xac, 0x8e, 0xbb, 0x86, 0x28, 0xa8, 0x75, 0xa5, 0x69, 0xa3, 0xf8, 0xa9, 0xbd, 0xe4 };
