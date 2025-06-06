// XXX ONLY FOR WEBSOCKET XXX -- SHA-1 is considered broken! nevertheless,
// SHA-1 is (pointlessly) needed by the WebSocket protocol; RFC6455 states:
// "The WebSocket handshake described in this document doesn't depend on any
// security properties of SHA-1, such as collision resistance or resistance to
// the second pre-image attack"

// public API for Steve Reid's public domain SHA-1 implementation
// this file is in the public domain

#ifndef SHA1_H

#include <stdint.h>

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    uint8_t  buffer[64];
} SHA1_CTX;

#define SHA1_DIGEST_SIZE 20

void SHA1_Init(SHA1_CTX* context);
void SHA1_Update(SHA1_CTX* context, const uint8_t* data, const size_t len);
void SHA1_Final(SHA1_CTX* context, uint8_t digest[SHA1_DIGEST_SIZE]);

#define SHA1_H
#endif
