#include "forge/sha256.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "forge_util.h"

/* ------------------------------------------------------------------ */
/* SHA-256 core (FIPS 180-4). Written fresh for forge so registry      */
/* downloads verify identically on every toolchain forge supports.     */
/* ------------------------------------------------------------------ */

typedef struct ForgeSha256 {
    uint32_t state[8];
    uint64_t length_bits;
    unsigned char block[64];
    size_t block_used;
} ForgeSha256;

static uint32_t rotr(uint32_t value, unsigned int bits)
{
    return (value >> bits) | (value << (32U - bits));
}

static void sha256_init(ForgeSha256 *context)
{
    context->state[0] = 0x6a09e667U;
    context->state[1] = 0xbb67ae85U;
    context->state[2] = 0x3c6ef372U;
    context->state[3] = 0xa54ff53aU;
    context->state[4] = 0x510e527fU;
    context->state[5] = 0x9b05688cU;
    context->state[6] = 0x1f83d9abU;
    context->state[7] = 0x5be0cd19U;
    context->length_bits = 0U;
    context->block_used = 0U;
}

static void sha256_compress(uint32_t state[8], const unsigned char *block)
{
    static const uint32_t round_keys[64] = {
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
        0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
        0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
        0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
        0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
        0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
        0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
        0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
        0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
        0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
        0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
        0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
        0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
    };
    uint32_t schedule[64];
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    uint32_t e;
    uint32_t f;
    uint32_t g;
    uint32_t h;
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        schedule[index] = ((uint32_t)block[index * 4U] << 24) |
                          ((uint32_t)block[index * 4U + 1U] << 16) |
                          ((uint32_t)block[index * 4U + 2U] << 8) |
                          (uint32_t)block[index * 4U + 3U];
    }
    for (index = 16U; index < 64U; ++index) {
        uint32_t s0 = rotr(schedule[index - 15U], 7U) ^
                      rotr(schedule[index - 15U], 18U) ^
                      (schedule[index - 15U] >> 3);
        uint32_t s1 = rotr(schedule[index - 2U], 17U) ^
                      rotr(schedule[index - 2U], 19U) ^
                      (schedule[index - 2U] >> 10);
        schedule[index] = schedule[index - 16U] + s0 +
                          schedule[index - 7U] + s1;
    }
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    f = state[5];
    g = state[6];
    h = state[7];
    for (index = 0U; index < 64U; ++index) {
        uint32_t s1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
        uint32_t choice = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + choice + round_keys[index] + schedule[index];
        uint32_t s0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
        uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

static void sha256_update(ForgeSha256 *context, const unsigned char *data,
                          size_t length)
{
    while (length > 0U) {
        size_t room = sizeof(context->block) - context->block_used;
        size_t take = length < room ? length : room;

        memcpy(context->block + context->block_used, data, take);
        context->block_used += take;
        context->length_bits += (uint64_t)take * 8U;
        data += take;
        length -= take;
        if (context->block_used == sizeof(context->block)) {
            sha256_compress(context->state, context->block);
            context->block_used = 0U;
        }
    }
}

static void sha256_final(ForgeSha256 *context, unsigned char *digest)
{
    static const unsigned char padding[64] = { 0x80 };
    unsigned char length_bytes[8];
    size_t index;
    int word;

    for (index = 0U; index < 8U; ++index) {
        length_bytes[index] =
            (unsigned char)(context->length_bits >> (56U - index * 8U));
    }
    /* Pad with 0x80 then zeroes until 56 bytes are used, then the length. */
    sha256_update(context, padding, 1U);
    while (context->block_used != 56U) {
        sha256_update(context, padding + 1U, 1U);
    }
    sha256_update(context, length_bytes, sizeof(length_bytes));
    for (word = 0; word < 8; ++word) {
        int byte;

        for (byte = 0; byte < 4; ++byte) {
            digest[word * 4 + byte] =
                (unsigned char)(context->state[word] >> (24U - (unsigned int)byte * 8U));
        }
    }
}

void forge_sha256_buffer(const unsigned char *data, size_t length,
                         char *hex_out)
{
    static const char digits[] = "0123456789abcdef";
    ForgeSha256 context;
    unsigned char digest[32];
    size_t index;

    sha256_init(&context);
    if (length > 0U && data != NULL) {
        sha256_update(&context, data, length);
    }
    sha256_final(&context, digest);
    for (index = 0U; index < sizeof(digest); ++index) {
        hex_out[index * 2U] = digits[digest[index] >> 4];
        hex_out[index * 2U + 1U] = digits[digest[index] & 0x0fU];
    }
    hex_out[sizeof(digest) * 2U] = '\0';
}

int forge_sha256_file(const char *path, char *hex_out,
                      char *error, size_t error_size)
{
    ForgeSha256 context;
    unsigned char digest[32];
    unsigned char chunk[8192];
    size_t read_bytes;
    FILE *file;

    if (path == NULL || hex_out == NULL) {
        forge_util_set_error(error, error_size, "sha256 needs a file path");
        return -1;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        forge_util_set_error(error, error_size, "cannot open '%s' for hashing",
                  path);
        return -1;
    }
    sha256_init(&context);
    do {
        read_bytes = fread(chunk, 1U, sizeof(chunk), file);
        if (read_bytes > 0U) {
            sha256_update(&context, chunk, read_bytes);
        }
    } while (read_bytes == sizeof(chunk));
    if (ferror(file)) {
        forge_util_set_error(error, error_size, "cannot read '%s' for hashing",
                  path);
        (void)fclose(file);
        return -1;
    }
    (void)fclose(file);
    sha256_final(&context, digest);
    {
        /* Hex-encode the digest (same alphabet as forge_sha256_buffer). */
        static const char digits[] = "0123456789abcdef";
        size_t index;

        for (index = 0U; index < sizeof(digest); ++index) {
            hex_out[index * 2U] = digits[digest[index] >> 4];
            hex_out[index * 2U + 1U] = digits[digest[index] & 0x0fU];
        }
        hex_out[sizeof(digest) * 2U] = '\0';
    }
    return 0;
}
