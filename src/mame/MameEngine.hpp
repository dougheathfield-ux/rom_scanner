#ifndef MAME_ENGINE_HPP
#define MAME_ENGINE_HPP

#include <iostream>
#include <filesystem>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <iomanip>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <functional>
#include "MameTypes.hpp"
#include "../core/CRC32Calculator.hpp"

namespace fs = std::filesystem;

class MameVerifier {
public:
    using LogCallback = std::function<void(const std::string&)>;

private:
    std::unordered_map<std::string, MachineEntry> machine_index;
    MameSetMode current_mode = MameSetMode::Split; 

    std::atomic<size_t> total_sets_checked{0};
    std::atomic<size_t> complete_sets{0};
    std::atomic<size_t> incomplete_sets{0};

    std::vector<MachineResultRecord> scanned_results;
    mutable std::mutex results_mutex;
    mutable std::mutex cout_mutex;

    LogCallback log_callback;

    void log_msg(const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(cout_mutex);
            std::cout << message;
        }
        if (log_callback) {
            log_callback(message);
        }
    }

    std::string extract_attribute(const std::string& line, const std::string& attr_name) {
        std::string target = attr_name + "=\"";
        size_t start = line.find(target);
        if (start == std::string::npos) return "";
        start += target.length();
        size_t end = line.find("\"", start);
        if (end == std::string::npos) return "";
        return line.substr(start, end - start);
    }

    std::string to_lower(std::string str) const {
        for (auto& c : str) c = std::tolower(c);
        return str;
    }

public:
    void set_log_callback(LogCallback cb) {
        log_callback = cb;
    }

    void set_mode(MameSetMode mode) {
        current_mode = mode;
    }

    const std::vector<MachineResultRecord>& get_results() const {
        std::lock_guard<std::mutex> lock(results_mutex);
        return scanned_results;
    }

    bool load_dat(const std::string& dat_filepath) {
        log_msg("[MameEngine] Loading XML DAT file: " + dat_filepath + "\n");
        
        std::ifstream file(dat_filepath);
        if (!file) {
            log_msg("[Warning] DAT file not found. Initializing mock fallback data.\n");
            MachineEntry parent;
            parent.name = "neogeo";
            parent.description = "Neo Geo BIOS";
            RomEntry bios_rom{"000-lo.lo", "", 131072, "5a86cff2", ""};
            parent.roms_by_crc["5a86cff2"] = bios_rom;
            parent.roms_by_name["000-lo.lo"] = bios_rom;
            machine_index["neogeo"] = parent;

            MachineEntry clone;
            clone.name = "mslug4";
            clone.description = "Metal Slug 4";
            clone.romof = "neogeo";
            RomEntry game_rom{"263-c1.c1", "", 8388608, "84865f8a", ""};
            clone.roms_by_crc["84865f8a"] = game_rom;
            clone.roms_by_name["263-c1.c1"] = game_rom;
            machine_index["mslug4"] = clone;
            return true;
        }

        std::string line;
        MachineEntry current_machine;
        bool inside_machine = false;
        size_t machine_count = 0;

        while (std::getline(file, line)) {
            if (line.find("<machine ") != std::string::npos || line.find("<game ") != std::string::npos) {
                current_machine = MachineEntry{};
                current_machine.name = extract_attribute(line, "name");
                current_machine.cloneof = extract_attribute(line, "cloneof");
                current_machine.romof = extract_attribute(line, "romof");
                inside_machine = true;
            }
            else if (inside_machine && line.find("<description>") != std::string::npos) {
                size_t start = line.find("<description>") + 13;
                size_t end = line.find("</description>");
                if (start != std::string::npos && end != std::string::npos) {
                    current_machine.description = line.substr(start, end - start);
                }
            }
            else if (inside_machine && line.find("<rom ") != std::string::npos) {
                RomEntry rom;
                rom.name = extract_attribute(line, "name");
                rom.merge = extract_attribute(line, "merge");
                
                std::string size_str = extract_attribute(line, "size");
                rom.size = size_str.empty() ? 0 : std::stoull(size_str);
                
                rom.crc32 = to_lower(extract_attribute(line, "crc"));

                if (!rom.crc32.empty()) {
                    current_machine.roms_by_crc[rom.crc32] = rom;
                    current_machine.roms_by_name[rom.name] = rom;
                }
            }
            else if (line.find("</machine>") != std::string::npos || line.find("</game>") != std::string::npos) {
                if (!current_machine.name.empty()) {
                    machine_index[current_machine.name] = current_machine;
                    machine_count++;
                }
                inside_machine = false;
            }
        }

        log_msg("[MameEngine] Successfully indexed " + std::to_string(machine_count) + " machines.\n");
        return true;
    }

    void evaluate_set(const std::string& machine_name, const std::unordered_set<std::string>& found_crcs) {
        auto it = machine_index.find(machine_name);
        if (it == machine_index.end()) return;

        total_sets_checked++;
        const auto& machine = it->second;

        std::unordered_map<std::string, RomEntry> required_roms = machine.roms_by_crc;

        if (current_mode == MameSetMode::Split && !machine.romof.empty()) {
            auto parent_it = machine_index.find(machine.romof);
            if (parent_it != machine_index.end()) {
                for (auto r_it = required_roms.begin(); r_it != required_roms.end();) {
                    if (r_it->second.is_shared()) {
                        r_it = required_roms.erase(r_it);
                    } else {
                        ++r_it;
                    }
                }
            }
        } 
        else if (current_mode == MameSetMode::NonMerged) {
            std::string current_parent = machine.romof;
            while (!current_parent.empty()) {
                auto parent_it = machine_index.find(current_parent);
                if (parent_it != machine_index.end()) {
                    for (const auto& [crc, rom] : parent_it->second.roms_by_crc) {
                        required_roms[crc] = rom;
                    }
                    current_parent = parent_it->second.romof;
                } else {
                    break;
                }
            }
        }
        else if (current_mode == MameSetMode::Merged) {
            for (const auto& [m_name, m_entry] : machine_index) {
                if (m_entry.romof == machine.name || m_entry.cloneof == machine.name) {
                    for (const auto& [crc, rom] : m_entry.roms_by_crc) {
                        required_roms[crc] = rom;
                    }
                }
            }
        }

        bool is_complete = true;
        std::vector<std::string> missing_roms;

        for (const auto& [crc, rom] : required_roms) {
            if (found_crcs.find(crc) == found_crcs.end()) {
                is_complete = false;
                missing_roms.push_back(rom.name);
            }
        }

        {
            std::lock_guard<std::mutex> r_lock(results_mutex);
            scanned_results.push_back({machine.name, machine.description, is_complete, missing_roms.size(), missing_roms});
        }

        log_msg("[Set Check] Machine: " + machine.name + " (" + machine.description + ")\n");
        if (is_complete) {
            complete_sets++;
            log_msg("  -> Status: [COMPLETE SET]\n");
        } else {
            incomplete_sets++;
            log_msg("  -> Status: [INCOMPLETE SET] Missing " + std::to_string(missing_roms.size()) + " ROM(s)\n");
            for (const auto& missing : missing_roms) {
                log_msg("     * Missing file: " + missing + "\n");
            }
        }
    }

    void print_summary() const {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "\n========================================\n"
                  << "      MAME SET VERIFICATION SUMMARY     \n"
                  << "========================================\n"
                  << "  Total Sets Evaluated : " << total_sets_checked.load() << "\n"
                  << "  Complete Sets        : " << complete_sets.load() << "\n"
                  << "  Incomplete / Missing : " << incomplete_sets.load() << "\n"
                  << "========================================\n";
    }

    void save_summary(const std::string& report_filepath) const {
        std::ofstream report(report_filepath);
        if (!report) {
            std::cerr << "[Error] Could not open report file for writing: " << report_filepath << "\n";
            return;
        }
        report << "========================================\n"
               << "      MAME SET VERIFICATION SUMMARY     \n"
               << "========================================\n"
               << "  Total Sets Evaluated : " << total_sets_checked.load() << "\n"
               << "  Complete Sets        : " << complete_sets.load() << "\n"
               << "  Incomplete / Missing : " << incomplete_sets.load() << "\n"
               << "========================================\n";
    }
};

#endif // MAME_ENGINE_HPP
