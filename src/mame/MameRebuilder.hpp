#ifndef MAME_REBUILDER_HPP
#define MAME_REBUILDER_HPP

#include <iostream>
#include <filesystem>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <iomanip>
#include <algorithm>
#include <memory>
#include <sstream>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <functional>
#include <archive.h>
#include <archive_entry.h>
#include "MameTypes.hpp"
#include "../core/CRC32Calculator.hpp"
#include "../core/ArchiveHandler.hpp"

namespace fs = std::filesystem;

struct SourceRomLocation {
    std::string container_path; 
    std::string internal_name;  
    bool is_archive;
};

class MameRebuilder {
public:
    using LogCallback = std::function<void(const std::string&)>;

private:
    std::unordered_map<std::string, MachineEntry> machine_index;
    MameSetMode current_mode = MameSetMode::Split;
    std::unordered_map<std::string, SourceRomLocation> global_rom_pool;
    
    std::atomic<size_t> total_sets_processed{0};
    std::atomic<size_t> successful_sets{0};
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

    bool fetch_rom_data(const SourceRomLocation& loc, std::vector<char>& out_buffer) {
        if (!loc.is_archive) {
            std::ifstream ifs(loc.container_path, std::ios::binary | std::ios::ate);
            if (!ifs) return false;
            std::streamsize size = ifs.tellg();
            ifs.seekg(0, std::ios::beg);
            out_buffer.resize(size);
            if (ifs.read(out_buffer.data(), size)) return true;
        } else {
            struct archive* a = archive_read_new();
            archive_read_support_format_all(a);
            archive_read_support_filter_all(a);

            if (archive_read_open_filename(a, loc.container_path.c_str(), 10240) == ARCHIVE_OK) {
                struct archive_entry* entry;
                while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
                    std::string current_name = archive_entry_pathname(entry);
                    if (current_name == loc.internal_name) {
                        int64_t size = archive_entry_size(entry);
                        out_buffer.resize(size);
                        int64_t read_bytes = archive_read_data(a, out_buffer.data(), size);
                        archive_read_free(a);
                        return read_bytes == size;
                    }
                }
            }
            archive_read_free(a);
        }
        return false;
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
        log_msg("[MameRebuilder] Loading DAT file: " + dat_filepath + "\n");
        std::ifstream file(dat_filepath);
        if (!file) {
            log_msg("[Error] DAT file not found: " + dat_filepath + "\n");
            return false;
        }

        std::string line;
        MachineEntry current_machine;
        bool inside_machine = false;

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
                }
                inside_machine = false;
            }
        }
        log_msg("[MameRebuilder] Indexed " + std::to_string(machine_index.size()) + " machines for rebuilding.\n");
        return true;
    }

    void index_source_directory(const std::string& source_dir) {
        log_msg("[MameRebuilder] Scavenging source directory: " + source_dir + "\n");
        
        for (const auto& entry : fs::recursive_directory_iterator(source_dir)) {
            if (!entry.is_regular_file()) continue;

            std::string path_str = entry.path().string();
            std::string ext = entry.path().extension().string();
            for (auto& c : ext) c = std::tolower(c);

            if (ext == ".zip" || ext == ".7z") {
                std::unique_ptr<CRC32Calculator> current_crc;
                std::string current_internal_name;

                auto finalize_current_file = [&]() {
                    if (current_crc) {
                        uint32_t final_hash = current_crc->finalize();
                        std::stringstream ss;
                        ss << std::hex << std::setw(8) << std::setfill('0') << final_hash;
                        std::string crc = to_lower(ss.str());
                        global_rom_pool[crc] = SourceRomLocation{path_str, current_internal_name, true};
                    }
                };

                ArchiveHandler::process_archive(
                    path_str,
                    [&](const std::string& internal_name) {
                        finalize_current_file();
                        current_internal_name = internal_name;
                        current_crc = std::make_unique<CRC32Calculator>();
                    },
                    [&](const char* data, size_t size) {
                        if (current_crc) current_crc->update(data, size);
                    }
                );
                finalize_current_file();
            } else {
                CRC32Calculator file_crc;
                std::ifstream ifs(path_str, std::ios::binary);
                char buffer[8192];
                while (ifs.read(buffer, sizeof(buffer))) {
                    file_crc.update(buffer, ifs.gcount());
                }
                if (ifs.gcount() > 0) file_crc.update(buffer, ifs.gcount());
                
                std::stringstream ss;
                ss << std::hex << std::setw(8) << std::setfill('0') << file_crc.finalize();
                global_rom_pool[to_lower(ss.str())] = SourceRomLocation{path_str, "", false};
            }
        }
        log_msg("[MameRebuilder] Scavenging complete. Pool size: " + std::to_string(global_rom_pool.size()) + " unique ROMs.\n");
    }

    bool rebuild_set(const std::string& machine_name, const std::string& output_dir) {
        auto it = machine_index.find(machine_name);
        if (it == machine_index.end()) return false;

        const auto& machine = it->second;
        std::unordered_map<std::string, RomEntry> required_roms = machine.roms_by_crc;

        if (current_mode == MameSetMode::Split && !machine.romof.empty()) {
            for (auto r_it = required_roms.begin(); r_it != required_roms.end();) {
                if (r_it->second.is_shared()) {
                    r_it = required_roms.erase(r_it);
                } else {
                    ++r_it;
                }
            }
        } else if (current_mode == MameSetMode::NonMerged) {
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
        } else if (current_mode == MameSetMode::Merged) {
            if (!machine.cloneof.empty()) {
                return true; 
            }
            for (const auto& [m_name, m_entry] : machine_index) {
                if (m_entry.cloneof == machine.name) {
                    for (const auto& [crc, rom] : m_entry.roms_by_crc) {
                        required_roms[crc] = rom;
                    }
                }
            }
        }

        if (required_roms.empty()) return true;

        size_t available_count = 0;
        for (const auto& [crc, rom] : required_roms) {
            if (global_rom_pool.find(crc) != global_rom_pool.end()) {
                available_count++;
            }
        }

        if (available_count == 0) {
            return false;
        }

        total_sets_processed++;

        fs::create_directories(output_dir);
        fs::path output_zip_path = fs::path(output_dir) / (machine.name + ".zip");

        struct archive* a = archive_write_new();
        archive_write_set_format_zip(a);
        if (archive_write_open_filename(a, output_zip_path.string().c_str()) != ARCHIVE_OK) {
            archive_write_free(a);
            return false;
        }

        bool all_roms_found = true;
        std::vector<std::string> missing_roms_vec;

        for (const auto& [crc, rom] : required_roms) {
            auto pool_it = global_rom_pool.find(crc);
            if (pool_it == global_rom_pool.end()) {
                all_roms_found = false;
                missing_roms_vec.push_back(rom.name);
                log_msg("  [-] Missing ROM for " + machine.name + ": " + rom.name + " (CRC: " + crc + ")\n");
                continue;
            }

            std::vector<char> rom_data;
            if (fetch_rom_data(pool_it->second, rom_data)) {
                struct archive_entry* entry = archive_entry_new();
                archive_entry_set_pathname(entry, rom.name.c_str());
                archive_entry_set_size(entry, rom_data.size());
                archive_entry_set_filetype(entry, AE_IFREG);
                archive_entry_set_perm(entry, 0644);

                archive_write_header(a, entry);
                archive_write_data(a, rom_data.data(), rom_data.size());
                archive_entry_free(entry);
            } else {
                all_roms_found = false;
                missing_roms_vec.push_back(rom.name);
                log_msg("  [-] Failed to read ROM data for: " + rom.name + "\n");
            }
        }

        archive_write_close(a);
        archive_write_free(a);

        {
            std::lock_guard<std::mutex> r_lock(results_mutex);
            scanned_results.push_back({machine.name, machine.description, all_roms_found, missing_roms_vec.size(), missing_roms_vec});
        }

        if (all_roms_found) {
            successful_sets++;
            log_msg("  [+] Successfully built set: " + machine.name + ".zip\n");
        } else {
            incomplete_sets++;
            log_msg("  [!] Built incomplete set: " + machine.name + ".zip (some ROMs missing)\n");
        }

        return all_roms_found;
    }

    void rebuild_all_sets(const std::string& output_dir, size_t num_threads = 0) {
        if (num_threads == 0) {
            num_threads = std::thread::hardware_concurrency();
            if (num_threads == 0) num_threads = 4;
        }

        log_msg("[MameRebuilder] Starting multithreaded batch rebuild using " + std::to_string(num_threads) + " worker threads into: " + output_dir + "\n");

        std::queue<std::string> task_queue;
        for (const auto& [name, machine] : machine_index) {
            if (!machine.roms_by_crc.empty()) {
                task_queue.push(name);
            }
        }

        std::mutex queue_mutex;
        std::atomic<size_t> completed_count{0};
        size_t total_tasks = task_queue.size();

        auto worker = [&]() {
            while (true) {
                std::string machine_name;
                {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    if (task_queue.empty()) return;
                    machine_name = task_queue.front();
                    task_queue.pop();
                }

                rebuild_set(machine_name, output_dir);

                size_t current_done = ++completed_count;
                log_msg("  [Progress] " + std::to_string(current_done) + " / " + std::to_string(total_tasks) + " sets processed.\n");
            }
        };

        std::vector<std::thread> pool;
        pool.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            pool.emplace_back(worker);
        }

        for (auto& t : pool) {
            if (t.joinable()) {
                t.join();
            }
        }

        log_msg("[MameRebuilder] Multithreaded batch rebuild complete!\n");
    }

    void print_summary() const {
        std::lock_guard<std::mutex> lock(cout_mutex);
        std::cout << "\n========================================\n"
                  << "      MAME REBUILD SUMMARY REPORT       \n"
                  << "========================================\n"
                  << "  Total Sets Processed : " << total_sets_processed.load() << "\n"
                  << "  Successful Sets      : " << successful_sets.load() << "\n"
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
               << "      MAME REBUILD SUMMARY REPORT       \n"
               << "========================================\n"
               << "  Total Sets Processed : " << total_sets_processed.load() << "\n"
               << "  Successful Sets      : " << successful_sets.load() << "\n"
               << "  Incomplete / Missing : " << incomplete_sets.load() << "\n"
               << "========================================\n";
    }
};

#endif // MAME_REBUILDER_HPP
