/* DEFLATE (RFC 1951) and gzip (RFC 1952) decoding, for data read from other releases' archives (steam_edges.c). */
#pragma once
#include <stddef.h>
#include <stdint.h>

/* Decodes the gzip member in[0..n-1] into out[0..out_n-1]. Returns the bytes written, or -1 when the data is not a
 * gzip member, is damaged (its CRC-32 or length does not match) or does not fit. */
long rt_gunzip(const uint8_t *in, size_t n, uint8_t *out, size_t out_n);
