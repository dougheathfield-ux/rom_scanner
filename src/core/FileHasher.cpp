#include "FileHasher.hpp"
#include <fstream>
#include <vector>
#include <iomanip>
#include <sstream>

// Suppress OpenSSL 3.0 deprecation warnings for low-level MD5/SHA1 APIs
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#elif defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4996)
#endif

#include <openssl/md5.h>
#include <openssl/sha.h>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace {
    // Standard IEEE 802.3 CRC32 table-driven calculator (self-contained)
    uint32_t calculate_crc32_block(uint32_t crc, const void* buf, size_t len) {
        static uint32_t table[256];
        static bool table_computed = false;
        if (!table_computed) {
            for (uint32_t i = 0; i < 256; i++) {
                uint32_t c = i;
                for (int j = 0; j < 8; j++) {
                    c = (c & 1) ? (0xEDB88320 ^ (c >> 1)) : (c >> 1);
                }
                table[i] = c;
            }
            table_computed = true;
        }

        crc = ~crc;
        const uint8_t* current = static_cast<const uint8_t*>(buf);
        for (size_t i = 0; i < len; i++) {
            crc = table[(crc ^ current[i]) & 0xFF] ^ (crc >> 8);
        }
        return ~crc;
    }

    std::string bytesToHex(const unsigned char* data, size_t len) {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0');
        for (size_t i = 0; i < len; ++i) {
            oss << std::setw(2) << static_cast<int>(data[i]);
        }
        return oss.str();
    }
}

// --- Streaming Context ---

FileHasher::Context::Context() : crc32Result(0) {
    auto md5 = new MD5_CTX();
    MD5_Init(md5);
    md5Context = md5;

    auto sha1 = new SHA_CTX();
    SHA1_Init(sha1);
    sha1Context = sha1;
}

FileHasher::Context::~Context() {
    delete static_cast<MD5_CTX*>(md5Context);
    delete static_cast<SHA_CTX*>(sha1Context);
}

void FileHasher::Context::update(const void* data, size_t size) {
    if (size == 0 || !data) return;

    crc32Result = calculate_crc32_block(crc32Result, data, size);
    MD5_Update(static_cast<MD5_CTX*>(md5Context), data, size);
    SHA1_Update(static_cast<SHA_CTX*>(sha1Context), data, size);
}

HashResult FileHasher::Context::finalize() {
    unsigned char md5Digest[MD5_DIGEST_LENGTH];
    MD5_Final(md5Digest, static_cast<MD5_CTX*>(md5Context));

    unsigned char sha1Digest[SHA_DIGEST_LENGTH];
    SHA1_Final(sha1Digest, static_cast<SHA_CTX*>(sha1Context));

    HashResult result;
    result.crc32 = crc32Result;
    result.md5 = bytesToHex(md5Digest, MD5_DIGEST_LENGTH);
    result.sha1 = bytesToHex(sha1Digest, SHA_DIGEST_LENGTH);

    return result;
}

// --- Local File Hashing ---

HashResult FileHasher::calculateFile(const std::string& filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        return {};
    }

    Context ctx;
    constexpr size_t bufferSize = 65536; // 64KB buffer
    std::vector<char> buffer(bufferSize);

    while (file.read(buffer.data(), bufferSize) || file.gcount() > 0) {
        ctx.update(buffer.data(), static_cast<size_t>(file.gcount()));
    }

    return ctx.finalize();
}
