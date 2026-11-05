
#include "sf2/crypto.hpp"

#include <sys/random.h>

#if defined(__ANDROID__)
#include <fcntl.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace sf2::crypto {
namespace {

constexpr uint32_t K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline uint32_t rotr(uint32_t x, int n) noexcept {
    return (x >> n) | (x << (32 - n));
}

}

void Sha256::reset() noexcept {
    h_[0] = 0x6a09e667u;
    h_[1] = 0xbb67ae85u;
    h_[2] = 0x3c6ef372u;
    h_[3] = 0xa54ff53au;
    h_[4] = 0x510e527fu;
    h_[5] = 0x9b05688cu;
    h_[6] = 0x1f83d9abu;
    h_[7] = 0x5be0cd19u;
    len_ = 0;
    total_ = 0;
}

void Sha256::block(const uint8_t *p) noexcept {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 |
               uint32_t(p[i * 4 + 2]) << 8 | uint32_t(p[i * 4 + 3]);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4], f = h_[5], g = h_[6],
             hh = h_[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h_[0] += a;
    h_[1] += b;
    h_[2] += c;
    h_[3] += d;
    h_[4] += e;
    h_[5] += f;
    h_[6] += g;
    h_[7] += hh;
}

void Sha256::update(const uint8_t *p, size_t n) noexcept {
    total_ += n;
    while (n) {
        size_t take = std::min(n, size_t(64) - len_);
        std::memcpy(buf_ + len_, p, take);
        len_ += take;
        p += take;
        n -= take;
        if (len_ == 64) {
            block(buf_);
            len_ = 0;
        }
    }
}

Digest Sha256::final_() noexcept {
    uint64_t bits = total_ * 8;
    uint8_t pad = 0x80;
    update(&pad, 1);
    uint8_t zero = 0;
    while (len_ != 56)
        update(&zero, 1);
    uint8_t lb[8];
    for (int i = 0; i < 8; ++i)
        lb[i] = uint8_t(bits >> (56 - 8 * i));
    update(lb, 8);
    Digest out{};
    for (int i = 0; i < 8; ++i) {
        out[size_t(i * 4)] = uint8_t(h_[i] >> 24);
        out[size_t(i * 4 + 1)] = uint8_t(h_[i] >> 16);
        out[size_t(i * 4 + 2)] = uint8_t(h_[i] >> 8);
        out[size_t(i * 4 + 3)] = uint8_t(h_[i]);
    }
    return out;
}

Digest sha256(ByteView v) noexcept {
    Sha256 s;
    s.update(v);
    return s.final_();
}

namespace {

inline uint32_t md5_rotl(uint32_t x, uint32_t n) noexcept {
    return (x << n) | (x >> (32 - n));
}

inline uint32_t md5_load_le(const uint8_t *p) noexcept {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

void md5_block(uint32_t st[4], const uint8_t block[64]) noexcept {
    static constexpr uint32_t S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                       5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                       6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    static constexpr uint32_t MD5_K[64] = {
        0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u,
        0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u,
        0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du,
        0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
        0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
        0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
        0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u,
        0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
        0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu,
        0xeb86d391u};

    uint32_t m[16];
    for (int i = 0; i < 16; ++i)
        m[i] = md5_load_le(block + i * 4);

    uint32_t a = st[0], b = st[1], c = st[2], d = st[3];
    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t f = 0, g = 0;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + md5_rotl(a + f + MD5_K[i] + m[g], S[i]);
        a = tmp;
    }
    st[0] += a;
    st[1] += b;
    st[2] += c;
    st[3] += d;
}

}

std::array<uint8_t, 16> md5(ByteView v) noexcept {
    uint32_t st[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};

    const uint8_t *p = v.data();
    size_t n = v.size();
    while (n >= 64) {
        md5_block(st, p);
        p += 64;
        n -= 64;
    }

    uint8_t tail[128]{};
    if (n)
        std::memcpy(tail, p, n);
    tail[n] = 0x80;
    size_t pad_len = (n < 56) ? 64 : 128;
    uint64_t bits = uint64_t(v.size()) * 8;
    for (int i = 0; i < 8; ++i)
        tail[pad_len - 8 + size_t(i)] = uint8_t(bits >> (8 * i));
    md5_block(st, tail);
    if (pad_len == 128)
        md5_block(st, tail + 64);

    std::array<uint8_t, 16> out{};
    for (int i = 0; i < 4; ++i) {
        out[size_t(i * 4)] = uint8_t(st[i]);
        out[size_t(i * 4 + 1)] = uint8_t(st[i] >> 8);
        out[size_t(i * 4 + 2)] = uint8_t(st[i] >> 16);
        out[size_t(i * 4 + 3)] = uint8_t(st[i] >> 24);
    }
    return out;
}

std::string md5_hex(std::string_view s) {
    auto d = md5(ByteView(reinterpret_cast<const uint8_t *>(s.data()), s.size()));
    return to_hex(ByteView(d.data(), d.size()));
}

Digest hmac_sha256(ByteView key, ByteView msg) noexcept {
    uint8_t k[64]{};
    if (key.size() > 64) {
        Digest d = sha256(key);
        std::memcpy(k, d.data(), 32);
    } else if (!key.empty()) {
        std::memcpy(k, key.data(), key.size());
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    Sha256 in;
    in.update(ipad, 64);
    in.update(msg);
    Digest ih = in.final_();
    Sha256 out;
    out.update(opad, 64);
    out.update(ih.data(), 32);
    return out.final_();
}

Digest pbkdf2(std::string_view password, ByteView salt, uint32_t iterations) noexcept {
    if (iterations == 0)
        iterations = 1;
    ByteView pw(reinterpret_cast<const uint8_t *>(password.data()), password.size());

    std::vector<uint8_t> block(salt.size() + 4);
    if (!salt.empty())
        std::memcpy(block.data(), salt.data(), salt.size());
    block[salt.size()] = 0;
    block[salt.size() + 1] = 0;
    block[salt.size() + 2] = 0;
    block[salt.size() + 3] = 1;

    Digest u = hmac_sha256(pw, ByteView(block.data(), block.size()));
    Digest acc = u;
    for (uint32_t i = 1; i < iterations; ++i) {
        u = hmac_sha256(pw, ByteView(u.data(), u.size()));
        for (size_t k = 0; k < 32; ++k)
            acc[k] ^= u[k];
    }
    return acc;
}

void random_bytes(uint8_t *out, size_t n) {
#if defined(__ANDROID__)

    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        throw std::runtime_error("open /dev/urandom failed");
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::read(fd, out + got, n - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            ::close(fd);
            throw std::runtime_error("read /dev/urandom failed");
        }
        if (r == 0) {
            ::close(fd);
            throw std::runtime_error("short read /dev/urandom");
        }
        got += size_t(r);
    }
    ::close(fd);
#else
    size_t got = 0;
    while (got < n) {
        ssize_t r = ::getrandom(out + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            throw std::runtime_error("getrandom failed");
        }
        got += size_t(r);
    }
#endif
}

std::string random_hex(size_t nbytes) {
    std::vector<uint8_t> b(nbytes);
    random_bytes(b.data(), nbytes);
    return to_hex(ByteView(b));
}

std::string to_hex(ByteView v) {
    static const char *H = "0123456789abcdef";
    std::string s;
    s.resize(v.size() * 2);
    for (size_t i = 0; i < v.size(); ++i) {
        s[i * 2] = H[v[i] >> 4];
        s[i * 2 + 1] = H[v[i] & 0xF];
    }
    return s;
}

bool from_hex(std::string_view hex, std::vector<uint8_t> &out) {
    if (hex.size() % 2)
        return false;
    out.clear();
    out.reserve(hex.size() / 2);
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = nib(hex[i]), lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) {
            out.clear();
            return false;
        }
        out.push_back(uint8_t(hi << 4 | lo));
    }
    return true;
}

bool const_time_eq(ByteView a, ByteView b) noexcept {
    if (a.size() != b.size())
        return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= uint8_t(a[i] ^ b[i]);
    return diff == 0;
}

}
