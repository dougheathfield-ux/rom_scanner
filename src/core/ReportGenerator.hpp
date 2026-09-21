#ifndef REPORT_GENERATOR_HPP
#define REPORT_GENERATOR_HPP

#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <sstream>
#include "../console/ConsoleEngine.hpp"

namespace fs = std::filesystem;

class ReportGenerator {
public:
    static void generate_reports(const std::vector<MachineResultRecord>& results, const fs::path& output_dir, const std::string& prefix) {
        fs::create_directories(output_dir);

        size_t total = results.size();
        size_t complete = 0;
        for (const auto& r : results) {
            if (r.is_complete) complete++;
        }
        size_t incomplete = total - complete;

        // 1. Text Summary Report (.txt)
        fs::path txt_path = output_dir / (prefix + "_report.txt");
        std::ofstream txt_out(txt_path);
        if (txt_out.is_open()) {
            txt_out << "===========================================\n";
            txt_out << "       ROM VERIFICATION & BUILD REPORT     \n";
            txt_out << "===========================================\n";
            txt_out << "Total Sets Processed : " << total << "\n";
            txt_out << "Complete Sets        : " << complete << "\n";
            txt_out << "Incomplete / Missing : " << incomplete << "\n";
            txt_out << "===========================================\n\n";
            txt_out << "DETAILED RESULTS:\n";
            txt_out << "-------------------------------------------\n";
            for (const auto& r : results) {
                txt_out << "[" << (r.is_complete ? "COMPLETE" : "MISSING") << "] " 
                        << r.name << " (" << r.description << ") - Missing ROMs: " << r.missing_count << "\n";
            }
        }

        // 2. JSON Structured Data Feed (.json)
        fs::path json_path = output_dir / (prefix + "_report.json");
        std::ofstream json_out(json_path);
        if (json_out.is_open()) {
            json_out << "{\n";
            json_out << "  \"total_sets\": " << total << ",\n";
            json_out << "  \"complete_sets\": " << complete << ",\n";
            json_out << "  \"incomplete_sets\": " << incomplete << ",\n";
            json_out << "  \"machines\": [\n";
            for (size_t i = 0; i < results.size(); ++i) {
                const auto& r = results[i];
                json_out << "    {\n";
                json_out << "      \"name\": \"" << r.name << "\",\n";
                json_out << "      \"description\": \"" << r.description << "\",\n";
                json_out << "      \"is_complete\": " << (r.is_complete ? "true" : "false") << ",\n";
                json_out << "      \"missing_count\": " << r.missing_count << "\n";
                json_out << "    }" << (i + 1 < results.size() ? "," : "") << "\n";
            }
            json_out << "  ]\n";
            json_out << "}\n";
        }

        // 3. HTML Dark-Mode Visual Dashboard (.html)
        fs::path html_path = output_dir / (prefix + "_dashboard.html");
        std::ofstream html_out(html_path);
        if (html_out.is_open()) {
            html_out << "<!DOCTYPE html>\n<html lang=\"en\">\n<head>\n";
            html_out << "<meta charset=\"UTF-8\">\n<title>ROM Manager Dashboard</title>\n";
            html_out << "<style>\n";
            html_out << "  body { background-color: #121212; color: #e0e0e0; font-family: Arial, sans-serif; margin: 0; padding: 20px; }\n";
            html_out << "  h1 { color: #bb86fc; }\n";
            html_out << "  .card { background: #1e1e1e; padding: 15px; margin-bottom: 20px; border-radius: 8px; box-shadow: 0 4px 6px rgba(0,0,0,0.3); }\n";
            html_out << "  table { width: 100%; border-collapse: collapse; margin-top: 15px; }\n";
            html_out << "  th, td { padding: 10px; border-bottom: 1px solid #333; text-align: left; }\n";
            html_out << "  th { background-color: #2d2d2d; color: #bb86fc; }\n";
            html_out << "  .complete { color: #03dac6; font-weight: bold; }\n";
            html_out << "  .incomplete { color: #cf6679; font-weight: bold; }\n";
            html_out << "</style>\n</head>\n<body>\n";
            html_out << "<h1>ROM Verification Dashboard</h1>\n";
            html_out << "<div class=\"card\">\n";
            html_out << "  <p><strong>Total Sets:</strong> " << total << "</p>\n";
            html_out << "  <p><strong>Complete Sets:</strong> <span class=\"complete\">" << complete << "</span></p>\n";
            html_out << "  <p><strong>Incomplete Sets:</strong> <span class=\"incomplete\">" << incomplete << "</span></p>\n";
            html_out << "</div>\n";
            html_out << "<div class=\"card\">\n  <h2>Machine Results</h2>\n  <table>\n";
            html_out << "    <tr><th>Machine</th><th>Description</th><th>Status</th><th>Missing ROMs</th></tr>\n";
            for (const auto& r : results) {
                html_out << "    <tr>\n";
                html_out << "      <td>" << r.name << "</td>\n";
                html_out << "      <td>" << r.description << "</td>\n";
                html_out << "      <td class=\"" << (r.is_complete ? "complete" : "incomplete") << "\">" << (r.is_complete ? "Complete" : "Incomplete") << "</td>\n";
                html_out << "      <td>" << r.missing_count << "</td>\n";
                html_out << "    </tr>\n";
            }
            html_out << "  </table>\n</div>\n</body>\n</html>\n";
        }

        // 4. Missing Checklist (.txt)
        fs::path checklist_path = output_dir / (prefix + "_missing_checklist.txt");
        std::ofstream check_out(checklist_path);
        if (check_out.is_open()) {
            check_out << "===========================================\n";
            check_out << "       MISSING ROMs CHECKLIST              \n";
            check_out << "===========================================\n\n";
            for (const auto& r : results) {
                if (!r.is_complete) {
                    check_out << "Machine: " << r.name << " (" << r.description << ")\n";
                    check_out << "  -> Missing files / ROMs count: " << r.missing_count << "\n\n";
                }
            }
        }
    }
};

#endif // REPORT_GENERATOR_HPP
