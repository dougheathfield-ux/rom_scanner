#pragma once
#include <string>
#include <cstdint>

struct HashResult {
    uint32_t crc32 = 0;
    std::string md5;
    std::string sha1;

    bool isEmpty() const {
        return crc32 == 0 && md5.empty() && sha1.empty();
    }
};
