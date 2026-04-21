#include "Uuid.h"

#include <cerrno>
#include <cstddef>

#ifdef __APPLE__
#include <stdlib.h>  // arc4random_buf
#else
#include <fcntl.h>
#include <sys/random.h>  // getrandom
#include <unistd.h>
#endif

static void fillSecureRandom(unsigned char* buf, size_t len)
{
#ifdef __APPLE__
    arc4random_buf(buf, len);
#else
    size_t off = 0;
    while (off < len) {
        ssize_t r = getrandom(buf + off, len - off, 0);
        if (r > 0) {
            off += static_cast<size_t>(r);
        } else if (r < 0 && errno != EINTR) {
            // getrandom unavailable (ancient kernel) — fall back to /dev/urandom.
            int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
            if (fd >= 0) {
                while (off < len) {
                    ssize_t n = ::read(fd, buf + off, len - off);
                    if (n > 0) off += static_cast<size_t>(n);
                    else if (n < 0 && errno == EINTR) continue;
                    else break;
                }
                ::close(fd);
            }
            break;
        }
    }
#endif
}

static uint64_t packBE(const unsigned char* b)
{
    return (static_cast<uint64_t>(b[0]) << 56) |
           (static_cast<uint64_t>(b[1]) << 48) |
           (static_cast<uint64_t>(b[2]) << 40) |
           (static_cast<uint64_t>(b[3]) << 32) |
           (static_cast<uint64_t>(b[4]) << 24) |
           (static_cast<uint64_t>(b[5]) << 16) |
           (static_cast<uint64_t>(b[6]) <<  8) |
           (static_cast<uint64_t>(b[7])      );
}

static void unpackBE(uint64_t v, unsigned char* b)
{
    b[0] = static_cast<unsigned char>(v >> 56);
    b[1] = static_cast<unsigned char>(v >> 48);
    b[2] = static_cast<unsigned char>(v >> 40);
    b[3] = static_cast<unsigned char>(v >> 32);
    b[4] = static_cast<unsigned char>(v >> 24);
    b[5] = static_cast<unsigned char>(v >> 16);
    b[6] = static_cast<unsigned char>(v >>  8);
    b[7] = static_cast<unsigned char>(v      );
}

Uuid Uuid::generate()
{
    unsigned char b[16];
    fillSecureRandom(b, sizeof(b));
    // Version 4: high nibble of byte 6 = 0100
    b[6] = (b[6] & 0x0F) | 0x40;
    // Variant 10xx: top two bits of byte 8
    b[8] = (b[8] & 0x3F) | 0x80;
    return Uuid{packBE(b), packBE(b + 8)};
}

std::string Uuid::toString() const
{
    unsigned char b[16];
    unpackBE(high, b);
    unpackBE(low,  b + 8);

    static const char hex[] = "0123456789abcdef";
    std::string out(36, '-');
    size_t p = 0;
    for (size_t i = 0; i < 16; ++i) {
        if (p == 8 || p == 13 || p == 18 || p == 23) ++p; // leave '-' in place
        out[p++] = hex[b[i] >> 4];
        out[p++] = hex[b[i] & 0x0F];
    }
    return out;
}

static int hexVal(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

Uuid Uuid::fromString(std::string_view s)
{
    if (s.size() != 36) return {};
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return {};
    unsigned char b[16];
    size_t p = 0;
    for (size_t i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        int hi = hexVal(s[i]);
        int lo = hexVal(s[++i]);
        if (hi < 0 || lo < 0) return {};
        b[p++] = static_cast<unsigned char>((hi << 4) | lo);
    }
    return Uuid{packBE(b), packBE(b + 8)};
}
