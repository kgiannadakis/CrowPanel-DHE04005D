#pragma once

#include <stdint.h>
#include <string.h>

#include <mbedtls/sha1.h>
#include <mbedtls/sha256.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void esp_sha(SHA_TYPE type, const uint8_t* input, size_t len, uint8_t* output) {
    if (!input || !output) return;

    switch (type) {
        case SHA1:
            mbedtls_sha1(input, len, output);
            break;
        case SHA2_224:
            mbedtls_sha256(input, len, output, 1);
            break;
        case SHA2_256:
            mbedtls_sha256(input, len, output, 0);
            break;
        default:
            memset(output, 0, 32);
            break;
    }
}

#ifdef __cplusplus
}
#endif
