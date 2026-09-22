#include <iostream>
#include <string>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <memory>
#include <vector>
#include <thread>
#include <atomic>
#include <unordered_set>
#include "console/ConsoleEngine.hpp"
#include "mame/MameEngine.hpp"
#include "mame/MameRebuilder.hpp"
#include "core/ArchiveHandler.hpp"
#include "core/FileHasher.hpp"
#include "core/DatManager.hpp"
#include "gui/GuiManager.hpp"

namespace fs = std::filesystem;

struct CliOptions {
    std::string dat_file;
    std::string target_path;
    std::string source_path;
    std::string engine_type = "console";
    std::string action = "scan";
    std::string mode_str = "split";
    size_t threads = 0;
    bool show_help = false;
};

CliOptions parse_cli_arguments(int argc, char* argv[]) {
    CliOptions options;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--dat" || arg == "-d") && i + 1 < argc) {
            options.dat_file = argv[++i];
        } else if ((arg == "--path" || arg == "-p") && i + 1 < argc) {
            options.target_path = argv[++i];
        } else if ((arg == "--source" || arg == "-s") && i + 1 < argc) {
            options.source_path = argv[++i];
        } else if ((arg == "--type" || arg == "-t") && i + 1 < argc) {
            options.engine_type = argv[++i];
        } else if ((arg == "--action" || arg == "-a") && i + 1 < argc) {
            options.action = argv[++i];
        } else if (arg == "--mode" && i + 1 < argc) {
            options.mode_str = argv[++i];
        } else if ((arg == "--threads" || arg == "-j") && i + 1 < argc) {
            options.threads = std::stoull(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            options.show_help = true;
        }
    }
    return options;
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n\n"
              << "If no options are provided, the Dear ImGui Graphical Interface will launch.\n\n"
              << "CLI Options:\n"
              << "  -a, --action <action> Action type: 'scan' (default) or 'rebuild'\n"
              << "  -p, --path <path>     Target scan directory OR output directory for rebuilt sets\n"
              << "  -s, --source <path>   Source ROM pool directory (required for rebuild action)\n"
              << "  -d, --dat <file>      Path to the XML DAT database file\n"
              << "  -t, --type <engine>   Engine type: 'console' or 'mame' (default: console)\n"
              << "      --mode <mode>     MAME set mode: 'split', 'nonmerged', or 'merged' (default: split)\n"
              << "  -j, --threads <num>   Number of worker threads for rebuilding (default: auto-detect)\n"
              << "  -h, --help            Display this help message\n";
}

int main(int argc, char* argv[]) {
    // If no arguments are passed, launch the GUI automatically!
    if (argc == 1) {
        GuiManager gui;
        if (!gui.init()) {
            std::cerr << "[Error] Failed to initialize Dear ImGui and GLFW window.\n";
            return -1;
        }
        std::cout << "[Info] GUI window launched successfully.\n";
        gui.run();
        gui.shutdown();
        return 0;
    }

    CliOptions opts = parse_cli_arguments(argc, argv);

    if (opts.show_help || (opts.dat_file.empty() && !opts.show_help)) {
        print_usage(argv[0]);
        return opts.show_help ? 0 : 1;
    }

    if (opts.engine_type == "mame") {
        if (opts.action == "rebuild") {
            std::cout << "[RomScanner] Initializing MAME Rebuilder Engine...\n";
            if (opts.source_path.empty()) {
                std::cerr << "[Error] Rebuilding requires a source ROM pool path specified via --source <path>\n";
                return 1;
            }

            MameRebuilder rebuilder;
            if (opts.mode_str == "nonmerged") rebuilder.set_mode(MameSetMode::NonMerged);
            else if (opts.mode_str == "merged") rebuilder.set_mode(MameSetMode::Merged);
            else rebuilder.set_mode(MameSetMode::Split);

            if (!rebuilder.load_dat(opts.dat_file)) return 1;

            rebuilder.index_source_directory(opts.source_path);
            rebuilder.rebuild_all_sets(opts.target_path, opts.threads);

        } else {
            std::cout << "[RomScanner] Initializing Hierarchical MAME Arcade Engine (Scan Mode)...\n";
            MameVerifier verifier;
            
            if (opts.mode_str == "nonmerged") verifier.set_mode(MameSetMode::NonMerged);
            else if (opts.mode_str == "merged") verifier.set_mode(MameSetMode::Merged);
            else verifier.set_mode(MameSetMode::Split);

            if (!verifier.load_dat(opts.dat_file)) return 1;

            fs::path target{opts.target_path};
            if (!fs::exists(target) || !fs::is_directory(target)) {
                std::cerr << "[Error] Target path is not a valid directory: " << opts.target_path << "\n";
                return 1;
            }

            for (const auto& entry : fs::directory_iterator(target)) {
                std::string machine_name = entry.path().stem().string();
                std::unordered_set<std::string> found_crcs;

                if (entry.is_regular_file()) {
                    std::string ext = entry.path().extension().string();
                    for (auto& c : ext) c = std::tolower(c);

                    if (ext == ".zip" || ext == ".7z") {
                        std::unique_ptr<FileHasher::Context> current_hasher;

                        ArchiveHandler::process_archive(
                            entry.path().string(),
                            [&](const std::string&) {
                                if (current_hasher) {
                                    HashResult res = current_hasher->finalize();
                                    std::stringstream ss;
                                    ss << std::hex << std::setw(8) << std::setfill('0') << res.crc32;
                                    std::string hex_str = ss.str();
                                    for (auto& c : hex_str) c = std::tolower(c);
                                    found_crcs.insert(hex_str);
                                }
                                current_hasher = std::make_unique<FileHasher::Context>();
                            },
                            [&](const char* data, size_t size) {
                                if (current_hasher) current_hasher->update(data, size);
                            }
                        );
                        if (current_hasher) {
                            HashResult res = current_hasher->finalize();
                            std::stringstream ss;
                            ss << std::hex << std::setw(8) << std::setfill('0') << res.crc32;
                            std::string hex_str = ss.str();
                            for (auto& c : hex_str) c = std::tolower(c);
                            found_crcs.insert(hex_str);
                        }
                    }
                } 
                else if (entry.is_directory()) {
                    for (const auto& file_entry : fs::recursive_directory_iterator(entry.path())) {
                        if (file_entry.is_regular_file()) {
                            HashResult res = FileHasher::calculateFile(file_entry.path().string());
                            
                            std::stringstream ss;
                            ss << std::hex << std::setw(8) << std::setfill('0') << res.crc32;
                            std::string crc = ss.str();
                            for (auto& c : crc) c = std::tolower(c);
                            found_crcs.insert(crc);
                        }
                    }
                }

                if (!found_crcs.empty()) {
                    verifier.evaluate_set(machine_name, found_crcs);
                }
            }

            verifier.print_summary();
        }
    } else {
        std::cout << "[RomScanner] Initializing Multithreaded Linux Console Engine...\n";
        ConsoleVerifier verifier;
        if (!verifier.load_dat(opts.dat_file)) return 1;
        
        fs::path target{opts.target_path};
        if (fs::exists(target) && fs::is_directory(target)) {
            for (const auto& entry : fs::recursive_directory_iterator(target)) {
                if (entry.is_regular_file()) {
                    verifier.verify_file(entry.path());
                }
            }
        }
        verifier.save_summary((fs::path(opts.target_path) / "../console_report.txt").string());
        verifier.print_summary();
    }

    std::cout << "[RomScanner] Operation complete!\n";
    return 0;
}
