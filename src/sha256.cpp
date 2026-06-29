#include "sha256.hpp"
#include <cstdio>

namespace {

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

const uint32_t K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

}

SHA256::SHA256() : total_(0), buflen_(0) {
	h_[0]=0x6a09e667; h_[1]=0xbb67ae85; h_[2]=0x3c6ef372; h_[3]=0xa54ff53a;
	h_[4]=0x510e527f; h_[5]=0x9b05688c; h_[6]=0x1f83d9ab; h_[7]=0x5be0cd19;
}

void SHA256::block(const uint8_t* p) {
	uint32_t w[64];
	for ( int i = 0; i < 16; i++ )
		w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
	for ( int i = 16; i < 64; i++ ) {
		uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
		uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19) ^ (w[i-2] >> 10);
		w[i] = w[i-16] + s0 + w[i-7] + s1;
	}
	uint32_t a=h_[0], b=h_[1], c=h_[2], d=h_[3], e=h_[4], f=h_[5], g=h_[6], k=h_[7];
	for ( int i = 0; i < 64; i++ ) {
		uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
		uint32_t ch = (e & f) ^ ((~e) & g);
		uint32_t t1 = k + S1 + ch + K[i] + w[i];
		uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
		uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
		uint32_t t2 = S0 + maj;
		k=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
	}
	h_[0]+=a; h_[1]+=b; h_[2]+=c; h_[3]+=d; h_[4]+=e; h_[5]+=f; h_[6]+=g; h_[7]+=k;
}

void SHA256::update(const void* data, size_t len) {
	const uint8_t* p = (const uint8_t*)data;
	total_ += len;
	while ( len > 0 ) {
		size_t take = 64 - buflen_;
		if ( take > len ) take = len;
		for ( size_t i = 0; i < take; i++ ) buf_[buflen_ + i] = p[i];
		buflen_ += take; p += take; len -= take;
		if ( buflen_ == 64 ) { block(buf_); buflen_ = 0; }
	}
}

std::string SHA256::hex() {
	uint64_t bitlen = total_ * 8;
	uint8_t pad = 0x80;
	update(&pad, 1);
	uint8_t zero = 0x00;
	while ( buflen_ != 56 ) update(&zero, 1);
	uint8_t lenbuf[8];
	for ( int i = 0; i < 8; i++ ) lenbuf[i] = (uint8_t)(bitlen >> ((7 - i) * 8));
	update(lenbuf, 8);   // triggers the final block

	static const char* hx = "0123456789abcdef";
	std::string out;
	out.reserve(64);
	for ( int i = 0; i < 8; i++ )
		for ( int j = 7; j >= 0; j-- )
			out += hx[(h_[i] >> (j * 4)) & 0xf];
	return out;
}

std::string sha256_hex(const std::string& data) {
	SHA256 s;
	s.update(data.data(), data.size());
	return s.hex();
}

std::string sha256_file(const std::string& path, std::string& err) {
	FILE* f = std::fopen(path.c_str(), "rb");
	if ( !f ) { err = "cannot open " + path; return ""; }
	SHA256 s;
	char buf[65536];
	size_t n;
	while (( n = std::fread(buf, 1, sizeof buf, f)) > 0 ) s.update(buf, n);
	bool ferr = std::ferror(f) != 0;
	std::fclose(f);
	if ( ferr ) { err = "read error on " + path; return ""; }
	return s.hex();
}
