#pragma once
#include <string>

// SHA-256 of `data`, returned as a lowercase hex string. Hand-rolled (FIPS
// 180-4) so the converter needs no OpenSSL/mbedTLS link just for digests.
std::string sha256_hex(const std::string& data);
