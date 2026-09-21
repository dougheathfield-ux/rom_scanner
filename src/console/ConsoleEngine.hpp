#ifndef CONSOLE_ENGINE_HPP
#define CONSOLE_ENGINE_HPP

#include <iostream>
#include <filesystem>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <string>
#include <atomic>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <functional>
#include <memory>
#include <algorithm>
#include <archive.h>
#include <archive_entry.h>
#include "../mame/MameTypes.hpp"
#include "../core/CRC32Calculator.hpp"
#include "../core/ArchiveHandler.hpp"

namespace fs = std::filesystem;

struct ConsoleRomEntry {
    std::string name;
    uint64_t size = 0;
    std::string crc32;
};

class ConsoleVerifier {
public:
    using LogCallback = std::function<void(const std::string&)>;

private:
    std::unordered_map<std::string, ConsoleRomEntry> dat_roms_by_crc;
    std::unordered_map<std::string, ConsoleRomEntry> dat_roms_by_name;

    std::atomic<size_t> total_files_checked{0};
    std::atomic<size_t> verified_files{0};
    std::atomic<size_t> missing_or_bad_files{0};

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

    bool is_auxiliary_file(const std::string& filename) const {
        std::string ext = fs::path(filename).extension().string();
        for (auto& c : ext) c = std::tolower(c);
        // .md is a valid Mega Drive ROM extension and has been removed from auxiliary files
        return (ext == ".txt" || ext == ".nfo" || ext == ".pdf" || ext == ".xml" || ext == ".diz");
    }

    // Helper to compute CRC32 string from data buffer
    std::string compute_crc32(const std::vector<char>& data) const {
        CRC32Calculator calc;
        if (!data.empty()) {
            calc.update(data.data(), data.size());
        }
        uint32_t final_hash = calc.finalize();
        std::stringstream ss;
        ss << std::hex << std::setw(8) << std::setfill('0') << final_hash;
        return to_lower(ss.str());
    }

    // Helper to deinterleave SMD (Super Magic Drive) block-swapped ROMs
    std::vector<char> deinterleave_smd(const std::vector<char>& input) const {
        std::vector<char> output = input;
        if (output.size() > 0 && output.size() % 16384 == 0) {
            size_t num_blocks = output.size() / 16384;
            std::vector<char> temp = output;
            for (size_t b = 0; b < num_blocks; ++b) {
                const char* src = temp.data() + (b * 16384);
                char* dest = output.data() + (b * 16384);
                for (size_t i = 0; i < 8192; ++i) {
                    dest[i * 2]     = src[8192 + i]; // even byte
                    dest[i * 2 + 1] = src[i];          // odd byte
                }
            }
        }
        return output;
    }

    // Test all common ROM variants against DAT database
    bool find_matching_rom(const std::vector<char>& raw_data, std::string& matched_name, std::string& matched_crc) {
        // Variant 1: Raw as-is
        std::string crc1 = compute_crc32(raw_data);
        if (dat_roms_by_crc.find(crc1) != dat_roms_by_crc.end()) {
            matched_crc = crc1;
            matched_name = dat_roms_by_crc[crc1].name;
            return true;
        }

        // Variant 2: Stripped 512-byte header (SMC/SWC/SMD header)
        if (raw_data.size() >= 512 && (raw_data.size() % 16384 == 512 || (raw_data.size() - 512) % 16384 == 0)) {
            std::vector<char> stripped(raw_data.begin() + 512, raw_data.end());
            std::string crc2 = compute_crc32(stripped);
            if (dat_roms_by_crc.find(crc2) != dat_roms_by_crc.end()) {
                matched_crc = crc2;
                matched_name = dat_roms_by_crc[crc2].name;
                return true;
            }

            // Variant 3: Stripped header + Deinterleaved
            std::vector<char> deint_stripped = deinterleave_smd(stripped);
            std::string crc3 = compute_crc32(deint_stripped);
            if (dat_roms_by_crc.find(crc3) != dat_roms_by_crc.end()) {
                matched_crc = crc3;
                matched_name = dat_roms_by_crc[crc3].name;
                return true;
            }
        }

        // Variant 4: Deinterleaved raw (without header stripping)
        if (raw_data.size() > 0 && raw_data.size() % 16384 == 0) {
            std::vector<char> deint_raw = deinterleave_smd(raw_data);
            std::string crc4 = compute_crc32(deint_raw);
            if (dat_roms_by_crc.find(crc4) != dat_roms_by_crc.end()) {
                matched_crc = crc4;
                matched_name = dat_roms_by_crc[crc4].name;
                return true;
            }
        }

        return false;
    }

public:
    void set_log_callback(LogCallback cb) {
        log_callback = cb;
    }

    const std::vector<MachineResultRecord>& get_results() const {
        std::lock_guard<std::mutex> lock(results_mutex);
        return scanned_results;
    }

    bool load_dat(const std::string& dat_filepath) {
        log_msg("[ConsoleEngine] Loading DAT file: " + dat_filepath + "\n");
        std::ifstream file(dat_filepath);
        if (!file) {
            log_msg("[Warning] Console DAT file not found. Initializing mock fallback data.\n");
            ConsoleRomEntry mock_rom{"Super Mario Bros.nes", 40960, "ea35f7bc"};
            dat_roms_by_crc["ea35f7bc"] = mock_rom;
            dat_roms_by_name["Super Mario Bros.nes"] = mock_rom;
            return true;
        }

        std::string line;
        std::string current_game_name = "";
        while (std::getline(file, line)) {
            if (line.find("<game ") != std::string::npos || line.find("<machine ") != std::string::npos) {
                current_game_name = extract_attribute(line, "name");
            }
            if (line.find("<rom ") != std::string::npos) {
                std::string crc = to_lower(extract_attribute(line, "crc"));
                std::string size_str = extract_attribute(line, "size");
                uint64_t size = size_str.empty() ? 0 : std::stoull(size_str);

                if (!crc.empty()) {
                    std::string rom_display_name = current_game_name.empty() ? extract_attribute(line, "name") : current_game_name;
                    ConsoleRomEntry rom{rom_display_name, size, crc};
                    dat_roms_by_crc[crc] = rom;
                    if (!rom_display_name.empty()) {
                        dat_roms_by_name[rom_display_name] = rom;
                    }
                }
            }
        }
        log_msg("[ConsoleEngine] Indexed " + std::to_string(dat_roms_by_crc.size()) + " console ROMs from DAT.\n");
        return true;
    }

    bool verify_file(const fs::path& filepath) {
        total_files_checked++;
        std::string filename = filepath.filename().string();
        std::string ext = filepath.extension().string();
        for (auto& c : ext) c = std::tolower(c);

        bool is_archive = (ext == ".zip" || ext == ".7z" || ext == ".rar");
        bool valid_match_found = false;
        std::string matched_rom_name = "";
        std::string matched_crc = "";

        if (is_archive) {
            auto check_rom_data = [&](const std::string& internal_name, const std::vector<char>& raw_data) {
                if (valid_match_found || is_auxiliary_file(internal_name)) return;
                if (find_matching_rom(raw_data, matched_rom_name, matched_crc)) {
                    valid_match_found = true;
                }
            };

            std::string current_internal_name;
            std::vector<char> current_file_buffer;

            ArchiveHandler::process_archive(
                filepath.string(),
                [&](const std::string& internal_name) {
                    if (!current_file_buffer.empty() || !current_internal_name.empty()) {
                        check_rom_data(current_internal_name, current_file_buffer);
                    }
                    current_internal_name = internal_name;
                    current_file_buffer.clear();
                },
                [&](const char* data, size_t size) {
                    current_file_buffer.insert(current_file_buffer.end(), data, data + size);
                }
            );
            if (!current_file_buffer.empty() || !current_internal_name.empty()) {
                check_rom_data(current_internal_name, current_file_buffer);
            }
        } else {
            std::ifstream ifs(filepath, std::ios::binary | std::ios::ate);
            if (!ifs) {
                missing_or_bad_files++;
                std::lock_guard<std::mutex> r_lock(results_mutex);
                scanned_results.push_back({filename, "Unreadable File", false, 1, {filename}});
                return false;
            }

            std::streamsize file_size = ifs.tellg();
            ifs.seekg(0, std::ios::beg);
            std::vector<char> file_data(file_size);
            if (ifs.read(file_data.data(), file_size)) {
                if (find_matching_rom(file_data, matched_rom_name, matched_crc)) {
                    valid_match_found = true;
                }
            }
        }

        {
            std::lock_guard<std::mutex> r_lock(results_mutex);
            if (valid_match_found) {
                scanned_results.push_back({filename, matched_rom_name.empty() ? "Console ROM" : matched_rom_name, true, 0, {}});
            } else {
                scanned_results.push_back({filename, "Unknown / Modified ROM", false, 1, {filename}});
            }
        }

        if (valid_match_found) {
            verified_files++;
            log_msg("  [+] Verified: " + filename + " -> " + matched_rom_name + " [CRC: " + matched_crc + "]\n");
        } else {
            missing_or_bad_files++;
            log_msg("  [-] Unmatched: " + filename + "\n");
        }

        return valid_match_found;
    }

    void print_summary() const {
        std::string summary = "\n========================================\n"
                              "     CONSOLE VERIFICATION SUMMARY       \n"
                              "========================================\n"
                              "  Total Files Checked  : " + std::to_string(total_files_checked.load()) + "\n"
                              "  Verified Files       : " + std::to_string(verified_files.load()) + "\n"
                              "  Unmatched / Bad      : " + std::to_string(missing_or_bad_files.load()) + "\n"
                              "========================================\n";
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << summary;
    }

    void save_summary(const std::string& report_filepath) const {
        std::ofstream report(report_filepath);
        if (!report) return;
        report << "========================================\n"
               << "     CONSOLE VERIFICATION SUMMARY       \n"
               << "========================================\n"
               << "  Total Files Checked  : " << total_files_checked.load() << "\n"
               << "  Verified Files       : " << verified_files.load() << "\n"
               << "  Unmatched / Bad      : " << missing_or_bad_files.load() << "\n"
               << "========================================\n";
    }
};

#endif // CONSOLE_ENGINE_HPP
