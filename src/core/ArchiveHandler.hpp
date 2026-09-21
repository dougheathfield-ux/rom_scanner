#ifndef ARCHIVE_HANDLER_HPP
#define ARCHIVE_HANDLER_HPP

#include <iostream>
#include <string>
#include <functional>
#include <archive.h>
#include <archive_entry.h>

class ArchiveHandler {
public:
    // Scans an archive (.zip, .7z, etc.) and streams each contained file's data block-by-block
    static bool process_archive(
        const std::string& archive_path,
        std::function<void(const std::string& internal_filename)> on_file_start,
        std::function<void(const char* data, size_t size)> on_file_data
    ) {
        struct archive *a = archive_read_new();
        archive_read_support_format_all(a);
        archive_read_support_filter_all(a);

        if (archive_read_open_filename(a, archive_path.c_str(), 10240) != ARCHIVE_OK) {
            std::cerr << "[ArchiveHandler] Failed to open archive: " << archive_path 
                      << " (" << archive_error_string(a) << ")\n";
            archive_read_free(a);
            return false;
        }

        struct archive_entry *entry;
        while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
            // Only process regular files inside the archive (skip directories)
            if (archive_entry_filetype(entry) & AE_IFREG) {
                std::string internal_name = archive_entry_pathname(entry);
                
                if (on_file_start) {
                    on_file_start(internal_name);
                }

                const void *buff;
                size_t size;
                la_int64_t offset;

                // Stream the internal file data chunks directly without writing to disk
                while (true) {
                    int r = archive_read_data_block(a, &buff, &size, &offset);
                    if (r == ARCHIVE_EOF) {
                        break;
                    }
                    if (r < ARCHIVE_OK) {
                        std::cerr << "[ArchiveHandler] Error reading file inside archive: " 
                                  << archive_error_string(a) << "\n";
                        break;
                    }
                    if (on_file_data && size > 0) {
                        on_file_data(static_cast<const char*>(buff), size);
                    }
                }
            }
        }

        archive_read_close(a);
        archive_read_free(a);
        return true;
    }
};

#endif // ARCHIVE_HANDLER_HPP
