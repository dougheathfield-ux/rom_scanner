#ifndef MAME_TYPES_HPP
#define MAME_TYPES_HPP

#include <string>
#include <vector>
#include <unordered_map>

enum class MameSetMode {
    NonMerged = 0,
    Split = 1,
    Merged = 2
};

struct RomEntry {
    std::string name;
    std::string merge;
    uint64_t size = 0;
    std::string crc32;
    std::string sha1;

    bool is_shared() const {
        return !merge.empty();
    }
};

struct MachineEntry {
    std::string name;
    std::string description;
    std::string cloneof;
    std::string romof;
    std::unordered_map<std::string, RomEntry> roms_by_crc;
    std::unordered_map<std::string, RomEntry> roms_by_name;
};

struct MachineResultRecord {
    std::string name;
    std::string description;
    bool is_complete;
    size_t missing_count;
    std::vector<std::string> missing_roms;
};

#endif // MAME_TYPES_HPP
