#pragma once
#include "HashResult.hpp"
#include <string>

class FileHasher {
public:
    static HashResult calculateFile(const std::string& filepath);

    class Context {
    public:
        Context();
        ~Context();

        void update(const void* data, size_t size);
        HashResult finalize();

    private:
        uint32_t crc32Result;
        void* md5Context;
        void* sha1Context;
    };
};
