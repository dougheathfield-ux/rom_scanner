#ifndef CONSOLE_REBUILDER_HPP
#define CONSOLE_REBUILDER_HPP

#include <iostream>
#include <filesystem>
#include <fstream>
#include <vector>
#include <unordered_map>
#include <string>
#include <iomanip>
#include <algorithm>
#include <memory>
#include <sstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <functional>
#include <archive.h>
#include <archive_entry.h>
#include "../mame/MameTypes.hpp"
#include "../core/CRC32Calculator.hpp"
#include "../core/ArchiveHandler.hpp"

namespace fs = std::filesystem;

struct ConsoleSourceLocation {
    std::string container_path;
    std::string internal_name;
    bool is_archive;
    int transformation_type = 0; // 0: Raw, 1: Stripped 512, 2: Deinterleaved, 3: Stripped + Deinterleaved
};

enum class ConsoleOutputFormat {
    Zip = 0,
    SevenZip = 1,
    Raw = 2
};

class ConsoleRebuilder {
public:
    using LogCallback = std::function<void(const std::string&)>;

private:
    struct ConsoleRomEntry {
        std::string name;
        std::string system_name = "Nintendo";
        std::string console_name = "N64";
        std::string region = "World";
        uint64_t size = 0;
        std::string crc32;
    };

    std::unordered_map<std::string, ConsoleRomEntry> dat_roms_by_crc;
    std::unordered_map<std::string, ConsoleSourceLocation> global_rom_pool;
    ConsoleOutputFormat output_format = ConsoleOutputFormat::Zip;
    bool use_region_folders = false;

    std::atomic<size_t> total_processed{0};
    std::atomic<size_t> successful_builds{0};
    std::atomic<size_t> failed_builds{0};

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
        return (ext == ".txt" || ext == ".nfo" || ext == ".pdf" || ext == ".xml" || ext == ".diz");
    }

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

    std::vector<char> deinterleave_smd(const std::vector<char>& input) const {
        std::vector<char> output = input;
        if (output.size() > 0 && output.size() % 16384 == 0) {
            size_t num_blocks = output.size() / 16384;
            std::vector<char> temp = output;
            for (size_t b = 0; b < num_blocks; ++b) {
                const char* src = temp.data() + (b * 16384);
                char* dest = output.data() + (b * 16384);
                for (size_t i = 0; i < 8192; ++i) {
                    dest[i * 2]     = src[8192 + i];
                    dest[i * 2 + 1] = src[i];
                }
            }
        }
        return output;
    }

    void index_variants(const std::string& container_path, const std::string& internal_name, bool is_archive, const std::vector<char>& raw_data) {
        // Variant 0: Raw
        std::string crc_raw = compute_crc32(raw_data);
        global_rom_pool[crc_raw] = ConsoleSourceLocation{container_path, internal_name, is_archive, 0};

        // Variant 1 & 3: With 512-byte header stripping
        if (raw_data.size() >= 512 && (raw_data.size() % 16384 == 512 || (raw_data.size() - 512) % 16384 == 0)) {
            std::vector<char> stripped(raw_data.begin() + 512, raw_data.end());
            std::string crc_strip = compute_crc32(stripped);
            global_rom_pool[crc_strip] = ConsoleSourceLocation{container_path, internal_name, is_archive, 1};

            std::vector<char> deint_strip = deinterleave_smd(stripped);
            std::string crc_deint_strip = compute_crc32(deint_strip);
            global_rom_pool[crc_deint_strip] = ConsoleSourceLocation{container_path, internal_name, is_archive, 3};
        }

        // Variant 2: Deinterleaved raw
        if (raw_data.size() > 0 && raw_data.size() % 16384 == 0) {
            std::vector<char> deint_raw = deinterleave_smd(raw_data);
            std::string crc_deint_raw = compute_crc32(deint_raw);
            global_rom_pool[crc_deint_raw] = ConsoleSourceLocation{container_path, internal_name, is_archive, 2};
        }
    }

    std::string parse_region(const std::string& name, const std::string& line) {
        std::string reg = extract_attribute(line, "region");
        if (!reg.empty()) return reg;

        size_t pos = 0;
        std::string detected_region = "World";
        bool found_specific = false;

        while ((pos = name.find('(', pos)) != std::string::npos) {
            size_t end = name.find(')', pos);
            if (end == std::string::npos) break;
            
            std::string inside = name.substr(pos + 1, end - pos - 1);
            std::string upper_inside = inside;
            for (auto& c : upper_inside) c = std::toupper(c);

            if (upper_inside.find("USA") != std::string::npos || upper_inside == "US") {
                detected_region = "USA";
                found_specific = true;
                break;
            }
            if (upper_inside.find("JAPAN") != std::string::npos || upper_inside == "JP" || upper_inside == "J") {
                detected_region = "Japan";
                found_specific = true;
                break;
            }
            if (upper_inside.find("EUROPE") != std::string::npos || upper_inside == "EU" || upper_inside == "PAL") {
                detected_region = "Europe";
                found_specific = true;
                break;
            }
            if (upper_inside.find("WORLD") != std::string::npos) {
                detected_region = "World";
                found_specific = true;
                break;
            }
            if (inside.find(',') != std::string::npos && 
                (inside.find("En") != std::string::npos || inside.find("Fr") != std::string::npos || inside.find("De") != std::string::npos)) {
                detected_region = "Europe";
                found_specific = true;
                break;
            }

            pos = end + 1;
        }

        if (found_specific) return detected_region;

        pos = 0;
        while ((pos = name.find('(', pos)) != std::string::npos) {
            size_t end = name.find(')', pos);
            if (end == std::string::npos) break;
            std::string inside = name.substr(pos + 1, end - pos - 1);
            if (inside.length() <= 4 && inside.find(',') == std::string::npos) {
                return inside;
            }
            pos = end + 1;
        }

        return "World";
    }

    bool fetch_rom_data(const ConsoleSourceLocation& loc, std::vector<char>& out_buffer) {
        std::vector<char> raw_data;

        if (!loc.is_archive) {
            std::ifstream ifs(loc.container_path, std::ios::binary | std::ios::ate);
            if (!ifs) return false;
            std::streamsize size = ifs.tellg();
            ifs.seekg(0, std::ios::beg);
            raw_data.resize(size);
            if (!ifs.read(raw_data.data(), size)) return false;
        } else {
            struct archive* a = archive_read_new();
            archive_read_support_format_all(a);
            archive_read_support_filter_all(a);

            bool found = false;
            if (archive_read_open_filename(a, loc.container_path.c_str(), 10240) == ARCHIVE_OK) {
                struct archive_entry* entry;
                while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
                    std::string current_name = archive_entry_pathname(entry);
                    if (current_name == loc.internal_name) {
                        int64_t size = archive_entry_size(entry);
                        raw_data.resize(size);
                        int64_t read_bytes = archive_read_data(a, raw_data.data(), size);
                        found = (read_bytes == size);
                        break;
                    }
                }
            }
            archive_read_free(a);
            if (!found) return false;
        }

        // Apply matching transformation to yield clean ROM data
        if (loc.transformation_type == 1 || loc.transformation_type == 3) {
            if (raw_data.size() >= 512) {
                raw_data.erase(raw_data.begin(), raw_data.begin() + 512);
            }
        }
        if (loc.transformation_type == 2 || loc.transformation_type == 3) {
            raw_data = deinterleave_smd(raw_data);
        }

        out_buffer = std::move(raw_data);
        return true;
    }

public:
    void set_log_callback(LogCallback cb) {
        log_callback = cb;
    }

    void set_output_format(ConsoleOutputFormat format) {
        output_format = format;
    }

    void set_use_region_folders(bool enable) {
        use_region_folders = enable;
    }

    const std::vector<MachineResultRecord>& get_results() const {
        std::lock_guard<std::mutex> lock(results_mutex);
        return scanned_results;
    }

    bool load_dat(const std::string& dat_filepath) {
        log_msg("[ConsoleRebuilder] Loading DAT file: " + dat_filepath + "\n");
        std::ifstream file(dat_filepath);
        if (!file) {
            log_msg("[Warning] DAT file not found. Initializing mock fallback data.\n");
            ConsoleRomEntry mock_rom{"Super Mario 64 (USA).z64", "Nintendo", "Nintendo 64", "USA", 33554432, "b198462a"};
            dat_roms_by_crc["b198462a"] = mock_rom;
            return true;
        }

        std::string line;
        bool in_header = false;
        std::string header_system = "Nintendo";
        std::string header_console = "N64";
        std::string current_game_name = "";

        while (std::getline(file, line)) {
            if (line.find("<header>") != std::string::npos) {
                in_header = true;
            }
            if (line.find("</header>") != std::string::npos) {
                in_header = false;
            }

            if (in_header && line.find("<name>") != std::string::npos) {
                size_t start = line.find("<name>") + 6;
                size_t end = line.find("</name>", start);
                if (start != std::string::npos && end != std::string::npos) {
                    std::string header_full_name = line.substr(start, end - start);
                    size_t hyphen = header_full_name.find(" - ");
                    if (hyphen != std::string::npos) {
                        header_system = header_full_name.substr(0, hyphen);
                        header_console = header_full_name.substr(hyphen + 3);
                    } else {
                        header_console = header_full_name;
                    }
                }
            }

            if (line.find("<game ") != std::string::npos || line.find("<machine ") != std::string::npos) {
                current_game_name = extract_attribute(line, "name");
            }

            if (line.find("<rom ") != std::string::npos) {
                std::string crc = to_lower(extract_attribute(line, "crc"));
                std::string size_str = extract_attribute(line, "size");
                uint64_t size = size_str.empty() ? 0 : std::stoull(size_str);
                std::string region = parse_region(current_game_name, line);

                if (!crc.empty()) {
                    ConsoleRomEntry rom{current_game_name.empty() ? "rom" : current_game_name, header_system, header_console, region, size, crc};
                    dat_roms_by_crc[crc] = rom;
                }
            }
        }
        log_msg("[ConsoleRebuilder] Indexed " + std::to_string(dat_roms_by_crc.size()) + " unique console ROMs for [" + header_system + " / " + header_console + "].\n");
        return true;
    }

    void index_source_directory(const std::string& source_dir) {
        log_msg("[ConsoleRebuilder] Scavenging source directory: " + source_dir + "\n");
        
        for (const auto& entry : fs::recursive_directory_iterator(source_dir)) {
            if (!entry.is_regular_file()) continue;

            std::string path_str = entry.path().string();
            std::string ext = entry.path().extension().string();
            for (auto& c : ext) c = std::tolower(c);

            if (ext == ".zip" || ext == ".7z" || ext == ".rar") {
                std::string current_internal_name;
                std::vector<char> current_file_buffer;

                auto finalize_archive_entry = [&]() {
                    if (!current_file_buffer.empty() && !current_internal_name.empty()) {
                        if (!is_auxiliary_file(current_internal_name)) {
                            index_variants(path_str, current_internal_name, true, current_file_buffer);
                        }
                    }
                };

                ArchiveHandler::process_archive(
                    path_str,
                    [&](const std::string& internal_name) {
                        finalize_archive_entry();
                        current_internal_name = internal_name;
                        current_file_buffer.clear();
                    },
                    [&](const char* data, size_t size) {
                        current_file_buffer.insert(current_file_buffer.end(), data, data + size);
                    }
                );
                finalize_archive_entry();
            } else {
                if (is_auxiliary_file(path_str)) continue;
                std::ifstream ifs(path_str, std::ios::binary | std::ios::ate);
                if (!ifs) continue;
                std::streamsize fsize = ifs.tellg();
                ifs.seekg(0, std::ios::beg);
                std::vector<char> file_data(fsize);
                if (ifs.read(file_data.data(), fsize)) {
                    index_variants(path_str, "", false, file_data);
                }
            }
        }
        log_msg("[ConsoleRebuilder] Source pool indexed. Total valid ROM hash variants: " + std::to_string(global_rom_pool.size()) + "\n");
    }

    bool rebuild_all(const std::string& output_base_dir) {
        log_msg("[ConsoleRebuilder] Starting hierarchical rebuild into: " + output_base_dir + "\n");

        for (const auto& [crc, rom] : dat_roms_by_crc) {
            total_processed++;
            auto pool_it = global_rom_pool.find(crc);

            if (pool_it == global_rom_pool.end()) {
                failed_builds++;
                std::lock_guard<std::mutex> r_lock(results_mutex);
                scanned_results.push_back({rom.name, rom.console_name, false, size_t(1), {rom.name}});
                log_msg("  [-] Missing ROM: " + rom.name + " (CRC: " + crc + ")\n");
                continue;
            }

            fs::path target_dir;
            if (use_region_folders) {
                target_dir = fs::path(output_base_dir) / rom.system_name / rom.console_name / rom.region;
            } else {
                target_dir = fs::path(output_base_dir) / rom.system_name / rom.console_name;
            }
            fs::create_directories(target_dir);

            std::string extension = ".zip";
            if (output_format == ConsoleOutputFormat::SevenZip) extension = ".7z";
            else if (output_format == ConsoleOutputFormat::Raw) extension = fs::path(rom.name).extension().string();

            fs::path target_file_path = target_dir / (fs::path(rom.name).stem().string() + extension);

            std::vector<char> rom_data;
            if (!fetch_rom_data(pool_it->second, rom_data)) {
                failed_builds++;
                std::lock_guard<std::mutex> r_lock(results_mutex);
                scanned_results.push_back({rom.name, rom.console_name, false, size_t(1), {rom.name}});
                log_msg("  [-] Failed to read data for: " + rom.name + "\n");
                continue;
            }

            bool write_success = false;
            if (output_format == ConsoleOutputFormat::Raw) {
                std::ofstream ofs(target_file_path, std::ios::binary);
                if (ofs.write(rom_data.data(), rom_data.size())) {
                    write_success = true;
                }
            } else {
                struct archive* a = archive_write_new();
                if (output_format == ConsoleOutputFormat::SevenZip) {
                    archive_write_set_format_7zip(a);
                } else {
                    archive_write_set_format_zip(a);
                }

                if (archive_write_open_filename(a, target_file_path.string().c_str()) == ARCHIVE_OK) {
                    struct archive_entry* entry = archive_entry_new();
                    archive_entry_set_pathname(entry, rom.name.c_str());
                    archive_entry_set_size(entry, rom_data.size());
                    archive_entry_set_filetype(entry, AE_IFREG);
                    archive_entry_set_perm(entry, 0644);

                    archive_write_header(a, entry);
                    archive_write_data(a, rom_data.data(), rom_data.size());
                    archive_entry_free(entry);
                    archive_write_close(a);
                    write_success = true;
                }
                archive_write_free(a);
            }

            {
                std::lock_guard<std::mutex> r_lock(results_mutex);
                scanned_results.push_back({rom.name, rom.console_name, write_success, write_success ? size_t(0) : size_t(1), write_success ? std::vector<std::string>{} : std::vector<std::string>{rom.name}});
            }

            if (write_success) {
                successful_builds++;
                log_msg("  [+] Built: " + target_file_path.string() + "\n");
            } else {
                failed_builds++;
                log_msg("  [!] Failed writing archive: " + target_file_path.string() + "\n");
            }
        }

        log_msg("[ConsoleRebuilder] Rebuild complete. Successful: " + std::to_string(successful_builds.load()) + ", Failed/Missing: " + std::to_string(failed_builds.load()) + "\n");
        return true;
    }

    void save_summary(const std::string& report_filepath) const {
        std::ofstream report(report_filepath);
        if (!report) return;
        report << "========================================\n"
               << "      CONSOLE REBUILD SUMMARY REPORT    \n"
               << "========================================\n"
               << "  Total Processed    : " << total_processed.load() << "\n"
               << "  Successful Builds  : " << successful_builds.load() << "\n"
               << "  Missing / Failed   : " << failed_builds.load() << "\n"
               << "========================================\n";
    }
};

#endif // CONSOLE_REBUILDER_HPP
