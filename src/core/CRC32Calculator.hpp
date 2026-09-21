#ifndef CRC32_CALCULATOR_HPP
#define CRC32_CALCULATOR_HPP

#include <cstdint>
#include <vector>

class CRC32Calculator {
private:
    uint32_t state;
    static uint32_t table[256];
    static bool table_initialized;

    static void init_table() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc = i;
            for (uint32_t j = 0; j < 8; j++) {
                if (crc & 1) {
                    crc = (crc >> 1) ^ 0xEDB88320;
                } else {
                    crc >>= 1;
                }
            }
            table[i] = crc;
        }
        table_initialized = true;
    }

public:
    CRC32Calculator() {
        if (!table_initialized) {
            init_table();
        }
        reset();
    }

    void reset() {
        state = 0xFFFFFFFF; // Initialize to all 1s
    }

    void update(const char* data, size_t length) {
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
        for (size_t i = 0; i < length; i++) {
            uint8_t index = (state ^ bytes[i]) & 0xFF;
            state = (state >> 8) ^ table[index];
        }
    }

    uint32_t finalize() const {
        return ~state; // Bitwise NOT at the end
    }
};

// Static member initialization (header-safe for C++20)
inline uint32_t CRC32Calculator::table[256];
inline bool CRC32Calculator::table_initialized = false;

#endif // CRC32_CALCULATOR_HPP
