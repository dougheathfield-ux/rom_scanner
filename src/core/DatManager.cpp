#include "DatManager.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>

namespace {
    // Helper to extract attribute value from an XML tag string (e.g., name="Super Mario")
    std::string getAttribute(const std::string& line, const std::string& attrName) {
        std::string target = attrName + "=\"";
        size_t start = line.find(target);
        if (start == std::string::npos) {
            // Try single quotes fallback
            target = attrName + "='";
            start = line.find(target);
            if (start == std::string::npos) return "";
        }
        start += target.length();
        size_t end = line.find(line[start - 1] == '"' ? '"' : '\'', start);
        if (end == std::string::npos) return "";
        return line.substr(start, end - start);
    }

    // Helper to extract inner text of an XML element (e.g., <version>1.0</version>)
    std::string getTagContent(const std::string& line, const std::string& tagName) {
        std::string openTag = "<" + tagName + ">";
        std::string closeTag = "</" + tagName + ">";
        size_t start = line.find(openTag);
        if (start == std::string::npos) return "";
        start += openTag.length();
        size_t end = line.find(closeTag, start);
        if (end == std::string::npos) return "";
        return line.substr(start, end - start);
    }

    // Convert hex string to uint32_t for CRC32
    uint32_t parseHexCrc(const std::string& hexStr) {
        if (hexStr.empty()) return 0;
        try {
            return static_cast<uint32_t>(std::stoul(hexStr, nullptr, 16));
        } catch (...) {
            return 0;
        }
    }
}

bool DatManager::loadDatFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "[DatManager] Failed to open DAT file: " << filepath << "\n";
        return false;
    }

    m_machines.clear();
    m_header = {};

    std::string line;
    bool inHeader = false;
    bool inMachine = false;
    DatMachine currentMachine;

    while (std::getline(file, line)) {
        // Basic trimming
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        if (line.empty()) continue;

        // Header parsing
        if (line.find("<header>") != std::string::npos) {
            inHeader = true;
            continue;
        }
        if (line.find("</header>") != std::string::npos) {
            inHeader = false;
            continue;
        }
        if (inHeader) {
            if (line.find("<name>") != std::string::npos) m_header.name = getTagContent(line, "name");
            else if (line.find("<description>") != std::string::npos) m_header.description = getTagContent(line, "description");
            else if (line.find("<version>") != std::string::npos) m_header.version = getTagContent(line, "version");
            else if (line.find("<author>") != std::string::npos) m_header.author = getTagContent(line, "author");
            continue;
        }

        // Machine / Game block tracking (Supports both MAME <machine> and ClrMAMEPro <game>)
        if (line.find("<machine") != std::string::npos || line.find("<game") != std::string::npos) {
            inMachine = true;
            currentMachine = {};
            currentMachine.name = getAttribute(line, "name");
            continue;
        }

        if ((line.find("</machine>") != std::string::npos || line.find("</game>") != std::string::npos) && inMachine) {
            m_machines.push_back(currentMachine);
            inMachine = false;
            continue;
        }

        if (inMachine) {
            if (line.find("<description>") != std::string::npos) {
                currentMachine.description = getTagContent(line, "description");
            } else if (line.find("<rom") != std::string::npos) {
                DatRomEntry rom;
                rom.name = getAttribute(line, "name");
                
                std::string sizeStr = getAttribute(line, "size");
                if (!sizeStr.empty()) {
                    try { rom.size = std::stoll(sizeStr); } catch (...) { rom.size = 0; }
                }

                rom.hashes.crc32 = parseHexCrc(getAttribute(line, "crc"));
                rom.hashes.md5 = getAttribute(line, "md5");
                rom.hashes.sha1 = getAttribute(line, "sha1");

                // Convert hashes to lowercase for uniform comparison
                std::transform(rom.hashes.md5.begin(), rom.hashes.md5.end(), rom.hashes.md5.begin(), ::tolower);
                std::transform(rom.hashes.sha1.begin(), rom.hashes.sha1.end(), rom.hashes.sha1.begin(), ::tolower);

                currentMachine.roms.push_back(rom);
            }
        }
    }

    std::cout << "[DatManager] Loaded DAT: '" << m_header.name 
              << "' with " << m_machines.size() << " machines/games parsed.\n";
    return true;
}

bool DatManager::checkForRemoteUpdate(const std::string& updateUrl, const std::string& downloadDestination) {
    if (updateUrl.empty()) return false;
    std::cout << "[DatManager] Checking for updates from: " << updateUrl << "\n";
    // Remote fetch implementation hook (can be expanded with libcurl)
    return true;
}
