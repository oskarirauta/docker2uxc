#pragma once
#include <string>
#include <cstdint>
#include <cstddef>

// Hand-rolled SHA-256 (FIPS 180-4) so the converter needs no OpenSSL/mbedTLS
// link just for digests. Incremental, so large layer blobs can be hashed while
// streaming instead of buffered whole in memory.
class SHA256 {
public:
	SHA256();
	void update(const void* data, size_t len);
	std::string hex();              // finalize and return the lowercase hex digest
private:
	void block(const uint8_t* p);
	uint32_t h_[8];
	uint64_t total_;
	uint8_t buf_[64];
	size_t buflen_;
};

std::string sha256_hex(const std::string& data);                       // one-shot
std::string sha256_file(const std::string& path, std::string& err);    // chunked (empty + err on failure)
