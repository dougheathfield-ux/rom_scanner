#pragma once
#include "HashResult.hpp"
#include <string>
#include <vector>
#include <unordered_map>

struct DatRomEntry {
    std::string name;
    int64_t size = 0;
    HashResult hashes;
};

struct DatMachine {
    std::string name;
    std::string description;
    std::vector<DatRomEntry> roms;
};

struct DatHeader {
    std::string name;
    std::string description;
    std::string version;
    std::string author;
};

class DatManager {
public:
    /**
     * Parses an XML DAT file (Logiqx/ClrMAMEPro format).
     */
    bool loadDatFile(const std::string& filepath);

    const DatHeader& getHeader() const { return m_header; }
    const std::vector<DatMachine>& getMachines() const { return m_machines; }

    /**
     * Optional lightweight hook to check for DAT updates or fetch remote metadata.
     */
    bool checkForRemoteUpdate(const std::string& updateUrl, const std::string& downloadDestination);

private:
    DatHeader m_header;
    std::vector<DatMachine> m_machines;
};
