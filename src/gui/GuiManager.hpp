#ifndef GUI_MANAGER_HPP
#define GUI_MANAGER_HPP

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <GLFW/glfw3.h>
#include <portable-file-dialogs.h>

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <iostream>
#include <filesystem>
#include <unordered_set>
#include <memory>
#include <functional>

#include "../console/ConsoleEngine.hpp"
#include "../console/ConsoleRebuilder.hpp"
#include "../mame/MameEngine.hpp"
#include "../mame/MameRebuilder.hpp"
#include "../core/ArchiveHandler.hpp"
#include "../core/CRC32Calculator.hpp"
#include "../core/ReportGenerator.hpp"

namespace fs = std::filesystem;

enum class AppMode {
    ConsoleVerifier,
    MameVerification,
    MameRebuilder,
    ConsoleRebuilder
};

class GuiManager {
private:
    GLFWwindow* window = nullptr;
    AppMode current_mode = AppMode::ConsoleVerifier;

    // Input state buffers
    char dat_path[512] = "datfiles/console.dat";
    char rom_dir[512] = "roms/console";
    char output_dir[512] = "output";
    char source_dir[512] = "roms/source_pool";

    int mame_mode_sel = 0; // 0: NonMerged, 1: Split, 2: Merged
    int console_format_sel = 0; // 0: Zip, 1: 7Z, 2: Raw
    bool console_region_folders = false; // Toggle for region-based subdirectories
    int thread_count = 4;

    // Execution state
    std::atomic<bool> is_running{false};
    std::vector<std::string> log_lines;
    std::mutex log_mutex;

    // Table search & filter state
    char search_filter[128] = "";
    int status_filter_idx = 0; // 0: All, 1: Complete, 2: Incomplete
    std::vector<MachineResultRecord> current_scan_results;
    std::mutex results_cache_mutex;

    void add_log(const std::string& line) {
        std::lock_guard<std::mutex> lock(log_mutex);
        log_lines.push_back(line);
        if (log_lines.size() > 2000) {
            log_lines.erase(log_lines.begin());
        }
    }

    void clear_logs() {
        std::lock_guard<std::mutex> lock(log_mutex);
        log_lines.clear();
        std::lock_guard<std::mutex> r_lock(results_cache_mutex);
        current_scan_results.clear();
    }

    void execute_operation() {
        is_running = true;
        clear_logs();

        add_log("[GUI] Starting operation...");

        try {
            fs::create_directories(output_dir);

            if (current_mode == AppMode::ConsoleVerifier) {
                add_log("[ConsoleEngine] Initializing multithreaded console verification...");
                ConsoleVerifier verifier;
                
                verifier.set_log_callback([this](const std::string& line) {
                    this->add_log(line);
                });

                if (!verifier.load_dat(dat_path)) {
                    add_log("[Error] Failed to load DAT file: " + std::string(dat_path));
                    is_running = false;
                    return;
                }

                if (fs::exists(rom_dir) && fs::is_directory(rom_dir)) {
                    for (const auto& entry : fs::recursive_directory_iterator(rom_dir)) {
                        if (entry.is_regular_file()) {
                            verifier.verify_file(entry.path());
                        }
                    }
                } else {
                    add_log("[Error] Target ROM directory does not exist: " + std::string(rom_dir));
                }

                fs::path out_path{output_dir};
                ReportGenerator::generate_reports(verifier.get_results(), out_path, "console");

                {
                    std::lock_guard<std::mutex> r_lock(results_cache_mutex);
                    current_scan_results = verifier.get_results();
                }

                add_log("[ConsoleEngine] Scan complete. Full report suite saved to: " + std::string(output_dir));
            }
            else if (current_mode == AppMode::MameVerification) {
                add_log("[MameEngine] Initializing Hierarchical MAME Arcade Scanner...");
                MameVerifier verifier;
                verifier.set_mode(static_cast<MameSetMode>(mame_mode_sel));

                verifier.set_log_callback([this](const std::string& line) {
                    this->add_log(line);
                });

                if (!verifier.load_dat(dat_path)) {
                    add_log("[Error] Failed to load MAME DAT file: " + std::string(dat_path));
                    is_running = false;
                    return;
                }

                fs::path target{rom_dir};
                if (!fs::exists(target) || !fs::is_directory(target)) {
                    add_log("[Error] Target path is not a valid directory: " + std::string(rom_dir));
                    is_running = false;
                    return;
                }

                for (const auto& entry : fs::directory_iterator(target)) {
                    std::string machine_name = entry.path().stem().string();
                    std::unordered_set<std::string> found_crcs;

                    if (entry.is_regular_file()) {
                        std::string ext = entry.path().extension().string();
                        for (auto& c : ext) c = std::tolower(c);

                        if (ext == ".zip" || ext == ".7z") {
                            std::unique_ptr<CRC32Calculator> current_crc;

                            ArchiveHandler::process_archive(
                                entry.path().string(),
                                [&](const std::string&) {
                                    if (current_crc) {
                                        uint32_t final_hash = current_crc->finalize();
                                        std::stringstream ss;
                                        ss << std::hex << std::setw(8) << std::setfill('0') << final_hash;
                                        std::string hex_str = ss.str();
                                        for (auto& c : hex_str) c = std::tolower(c);
                                        found_crcs.insert(hex_str);
                                    }
                                    current_crc = std::make_unique<CRC32Calculator>();
                                },
                                [&](const char* data, size_t size) {
                                    if (current_crc) current_crc->update(data, size);
                                }
                            );
                            if (current_crc) {
                                uint32_t final_hash = current_crc->finalize();
                                std::stringstream ss;
                                ss << std::hex << std::setw(8) << std::setfill('0') << final_hash;
                                std::string hex_str = ss.str();
                                for (auto& c : hex_str) c = std::tolower(c);
                                found_crcs.insert(hex_str);
                            }
                        }
                    } 
                    else if (entry.is_directory()) {
                        for (const auto& file_entry : fs::recursive_directory_iterator(entry.path())) {
                            if (file_entry.is_regular_file()) {
                                CRC32Calculator file_crc;
                                std::ifstream ifs(file_entry.path(), std::ios::binary);
                                char buffer[8192];
                                while (ifs.read(buffer, sizeof(buffer))) {
                                    file_crc.update(buffer, ifs.gcount());
                                }
                                if (ifs.gcount() > 0) {
                                    file_crc.update(buffer, ifs.gcount());
                                }
                                uint32_t final_hash = file_crc.finalize();
                                
                                std::stringstream ss;
                                ss << std::hex << std::setw(8) << std::setfill('0') << final_hash;
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
                
                {
                    std::lock_guard<std::mutex> r_lock(results_cache_mutex);
                    current_scan_results = verifier.get_results();
                }

                add_log("[MameEngine] Verification check finished.");
            }
            else if (current_mode == AppMode::MameRebuilder) {
                add_log("[MameRebuilder] Initializing MAME Rebuilder Engine...");
                if (std::string(source_dir).empty()) {
                    add_log("[Error] Rebuilding requires a valid source ROM pool path.");
                    is_running = false;
                    return;
                }

                MameRebuilder rebuilder;
                rebuilder.set_mode(static_cast<MameSetMode>(mame_mode_sel));

                rebuilder.set_log_callback([this](const std::string& line) {
                    this->add_log(line);
                });

                if (!rebuilder.load_dat(dat_path)) {
                    add_log("[Error] Failed to load MAME DAT file.");
                    is_running = false;
                    return;
                }

                rebuilder.index_source_directory(source_dir);
                rebuilder.rebuild_all_sets(output_dir, thread_count);

                {
                    std::lock_guard<std::mutex> r_lock(results_cache_mutex);
                    current_scan_results = rebuilder.get_results();
                }

                fs::path out_path{output_dir};
                ReportGenerator::generate_reports(rebuilder.get_results(), out_path, "mame_rebuild");
                add_log("[MameRebuilder] Batch rebuild completed successfully. Report saved to: " + std::string(output_dir));
            }
            else if (current_mode == AppMode::ConsoleRebuilder) {
                add_log("[ConsoleRebuilder] Initializing Console Rebuilder Engine...");
                if (std::string(source_dir).empty()) {
                    add_log("[Error] Rebuilding console ROMs requires a source ROM pool directory.");
                    is_running = false;
                    return;
                }

                ConsoleRebuilder rebuilder;
                rebuilder.set_output_format(static_cast<ConsoleOutputFormat>(console_format_sel));
                rebuilder.set_use_region_folders(console_region_folders);

                rebuilder.set_log_callback([this](const std::string& line) {
                    this->add_log(line);
                });

                if (!rebuilder.load_dat(dat_path)) {
                    add_log("[Error] Failed to load Console DAT file.");
                    is_running = false;
                    return;
                }

                rebuilder.index_source_directory(source_dir);
                rebuilder.rebuild_all(output_dir);

                {
                    std::lock_guard<std::mutex> r_lock(results_cache_mutex);
                    current_scan_results = rebuilder.get_results();
                }

                fs::path out_path{output_dir};
                ReportGenerator::generate_reports(rebuilder.get_results(), out_path, "console_rebuild");
                add_log("[ConsoleRebuilder] Hierarchical rebuild complete. Full report suite saved to: " + std::string(output_dir));
            }
        } catch (const std::exception& e) {
            add_log(std::string("[Exception] ") + e.what());
        }

        is_running = false;
        add_log("[GUI] Operation complete!");
    }

public:
    bool init() {
        if (!glfwInit()) return false;

        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        window = glfwCreateWindow(1350, 780, "ROM Manager & Verifier Suite", nullptr, nullptr);
        if (!window) return false;

        glfwMakeContextCurrent(window);
        glfwSwapInterval(1);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

        ImGui::StyleColorsDark();

        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 130");

        return true;
    }

    void run() {
        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();

            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);

            ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | 
                                            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | 
                                            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus | 
                                            ImGuiWindowFlags_NoNavFocus;

            ImGui::Begin("MainWorkspace", nullptr, window_flags);

            // --- 1. Left Sidebar (Mode Selector) ---
            ImGui::BeginChild("Sidebar", ImVec2(220, 0), true);
            ImGui::Text("ROM Manager Suite");
            ImGui::Separator();
            ImGui::Spacing();

            // Console Group
            ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "Console Tools");
            ImGui::Spacing();
            if (ImGui::Selectable("1. Console Verification", current_mode == AppMode::ConsoleVerifier)) {
                current_mode = AppMode::ConsoleVerifier;
            }
            if (ImGui::Selectable("2. Console Rebuilder", current_mode == AppMode::ConsoleRebuilder)) {
                current_mode = AppMode::ConsoleRebuilder;
            }

            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();

            // MAME Arcade Group
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f), "MAME Arcade Tools");
            ImGui::Spacing();
            if (ImGui::Selectable("3. MAME Verification", current_mode == AppMode::MameVerification)) {
                current_mode = AppMode::MameVerification;
            }
            if (ImGui::Selectable("4. MAME Rebuilder", current_mode == AppMode::MameRebuilder)) {
                current_mode = AppMode::MameRebuilder;
            }

            ImGui::EndChild();

            ImGui::SameLine();

            // --- 2. Main Content Split ---
            float main_content_width = ImGui::GetContentRegionAvail().x;
            float left_panel_width = main_content_width * 0.33f;

            // 1/3 Options Panel
            ImGui::BeginChild("OptionsPanel", ImVec2(left_panel_width, 0), true);
            ImGui::Text("Operation Settings");
            ImGui::Separator();

            float avail = ImGui::GetContentRegionAvail().x;
            float btn_w = 65.0f;

            // DAT File Path
            ImGui::Text("DAT File Path:");
            ImGui::SetNextItemWidth(avail - btn_w - 6.0f);
            ImGui::InputText("##datpath", dat_path, sizeof(dat_path));
            ImGui::SameLine();
            if (ImGui::Button("Browse##dat", ImVec2(btn_w, 0))) {
                auto selection = pfd::open_file("Select DAT File", "", {"DAT Files", "*.dat", "All Files", "*"}).result();
                if (!selection.empty()) {
                    snprintf(dat_path, sizeof(dat_path), "%s", selection[0].c_str());
                }
            }

            // Target ROM Directory (for Verifiers)
            if (current_mode == AppMode::ConsoleVerifier || current_mode == AppMode::MameVerification) {
                ImGui::Text("Target ROM Directory:");
                ImGui::SetNextItemWidth(avail - btn_w - 6.0f);
                ImGui::InputText("##romdir", rom_dir, sizeof(rom_dir));
                ImGui::SameLine();
                if (ImGui::Button("Browse##rom", ImVec2(btn_w, 0))) {
                    auto selection = pfd::select_folder("Select Target ROM Directory").result();
                    if (!selection.empty()) {
                        snprintf(rom_dir, sizeof(rom_dir), "%s", selection.c_str());
                    }
                }
            }

            // Source ROM Pool Dir (for Rebuilders)
            if (current_mode == AppMode::MameRebuilder || current_mode == AppMode::ConsoleRebuilder) {
                ImGui::Text("Source ROM Pool Dir:");
                ImGui::SetNextItemWidth(avail - btn_w - 6.0f);
                ImGui::InputText("##sourcedir", source_dir, sizeof(source_dir));
                ImGui::SameLine();
                if (ImGui::Button("Browse##source", ImVec2(btn_w, 0))) {
                    auto selection = pfd::select_folder("Select Source ROM Pool Directory").result();
                    if (!selection.empty()) {
                        snprintf(source_dir, sizeof(source_dir), "%s", selection.c_str());
                    }
                }
            }

            // Output Directory
            ImGui::Text("Output Directory:");
            ImGui::SetNextItemWidth(avail - btn_w - 6.0f);
            ImGui::InputText("##outdir", output_dir, sizeof(output_dir));
            ImGui::SameLine();
            if (ImGui::Button("Browse##out", ImVec2(btn_w, 0))) {
                auto selection = pfd::select_folder("Select Output Directory").result();
                if (!selection.empty()) {
                    snprintf(output_dir, sizeof(output_dir), "%s", selection.c_str());
                }
            }

            if (current_mode == AppMode::MameVerification || current_mode == AppMode::MameRebuilder) {
                ImGui::Separator();
                ImGui::Text("MAME Set Mode:");
                ImGui::RadioButton("Non-Merged Sets", &mame_mode_sel, 0);
                ImGui::RadioButton("Split Sets", &mame_mode_sel, 1);
                ImGui::RadioButton("Merged Sets", &mame_mode_sel, 2);
            }

            if (current_mode == AppMode::ConsoleRebuilder) {
                ImGui::Separator();
                ImGui::Text("Console Output Format:");
                ImGui::RadioButton("ZIP Archive (.zip)", &console_format_sel, 0);
                ImGui::RadioButton("7-Zip Archive (.7z)", &console_format_sel, 1);
                ImGui::RadioButton("Uncompressed Raw", &console_format_sel, 2);

                ImGui::Spacing();
                ImGui::Checkbox("Organize into Region Folders", &console_region_folders);
            }

            if (current_mode == AppMode::MameRebuilder) {
                ImGui::Separator();
                ImGui::Text("Worker Threads:");
                ImGui::SliderInt("##threads", &thread_count, 1, 16);
            }

            ImGui::Separator();
            if (is_running) {
                ImGui::Button("Running...", ImVec2(-1, 40));
            } else {
                if (ImGui::Button("Start Operation", ImVec2(-1, 40))) {
                    std::thread(&GuiManager::execute_operation, this).detach();
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();

            // 2/3 Tabbed Output / Results Table & Live Logs Panel
            ImGui::BeginChild("OutputPanel", ImVec2(0, 0), true);
            if (ImGui::BeginTabBar("OutputTabs")) {
                
                // TAB 1: Structured Results Table
                if (ImGui::BeginTabItem("Results Table")) {
                    ImGui::Spacing();
                    ImGui::SetNextItemWidth(220.0f);
                    ImGui::InputTextWithHint("##search", "Search Machine...", search_filter, sizeof(search_filter));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(150.0f);
                    ImGui::Combo("##statusfilter", &status_filter_idx, "All Status\0Complete Only\0Incomplete Only\0");

                    ImGui::Separator();

                    ImGuiTableFlags table_flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | 
                                                  ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable;
                    
                    if (ImGui::BeginTable("ScanResultsTable", 4, table_flags, ImVec2(0, 0))) {
                        ImGui::TableSetupColumn("Machine", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 100.0f);
                        ImGui::TableSetupColumn("Missing ROMs", ImGuiTableColumnFlags_WidthFixed, 90.0f);
                        ImGui::TableHeadersRow();

                        std::lock_guard<std::mutex> r_lock(results_cache_mutex);
                        for (const auto& res : current_scan_results) {
                            std::string query(search_filter);
                            if (!query.empty()) {
                                std::string m_name = res.name;
                                std::string m_desc = res.description;
                                for (auto& c : query) c = std::tolower(c);
                                for (auto& c : m_name) c = std::tolower(c);
                                for (auto& c : m_desc) c = std::tolower(c);
                                if (m_name.find(query) == std::string::npos && m_desc.find(query) == std::string::npos) {
                                    continue;
                                }
                            }

                            if (status_filter_idx == 1 && !res.is_complete) continue;
                            if (status_filter_idx == 2 && res.is_complete) continue;

                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);
                            ImGui::Text("%s", res.name.c_str());

                            ImGui::TableSetColumnIndex(1);
                            ImGui::Text("%s", res.description.c_str());

                            ImGui::TableSetColumnIndex(2);
                            if (res.is_complete) {
                                ImGui::TextColored(ImVec4(0.2f, 0.8f, 0.2f, 1.0f), "Complete");
                            } else {
                                ImGui::TextColored(ImVec4(0.9f, 0.3f, 0.3f, 1.0f), "Incomplete");
                            }

                            ImGui::TableSetColumnIndex(3);
                            ImGui::Text("%zu", res.missing_count);
                        }
                        ImGui::EndTable();
                    }
                    ImGui::EndTabItem();
                }

                // TAB 2: Live Logs
                if (ImGui::BeginTabItem("Live Logs")) {
                    ImGui::Spacing();
                    ImGui::BeginChild("LogScrollingRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
                    {
                        std::lock_guard<std::mutex> lock(log_mutex);
                        for (const auto& line : log_lines) {
                            ImGui::TextUnformatted(line.c_str());
                        }
                    }
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }
            ImGui::EndChild();

            ImGui::End(); // MainWorkspace

            ImGui::Render();
            int display_w, display_h;
            glfwGetFramebufferSize(window, &display_w, &display_h);
            glViewport(0, 0, display_w, display_h);
            glClearColor(0.12f, 0.12f, 0.14f, 1.00f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

            glfwSwapBuffers(window);
        }
    }

    void shutdown() {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();

        if (window) {
            glfwDestroyWindow(window);
        }
        glfwTerminate();
    }
};

#endif // GUI_MANAGER_HPP
