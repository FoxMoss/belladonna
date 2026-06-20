
#include "diffgen.h"

#include <archive.h>
#include <archive_entry.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

extern "C" {
void diffgen(const char* changed_dir, const char* truth_dir) {
  std::filesystem::recursive_directory_iterator iter(changed_dir);

  auto archive = archive_write_new();
  archive_write_add_filter_gzip(archive);
  archive_write_set_format_ustar(archive);

  archive_write_open_filename(archive, "upatch.tar.gz");

  auto old_path = std::filesystem::current_path();
  std::filesystem::current_path(changed_dir);

  for (auto file : iter) {
    if (file.is_directory()) {
      continue;
    }

    printf("%s\n",
           file.path().string().substr(strlen(changed_dir), -1).c_str());

    struct archive* ard = archive_read_disk_new();
    archive_read_disk_set_standard_lookup(ard);
    struct archive_entry* entry = archive_entry_new();
    int fd = open(file.path().c_str(), O_RDONLY);
    if (fd < 0) continue;
    archive_entry_copy_pathname(
        entry, file.path().string().substr(strlen(changed_dir), -1).c_str());
    archive_read_disk_entry_from_file(ard, entry, fd, nullptr);
    archive_write_header(archive, entry);
    char buf[8192];
    size_t bytes_read;

    while ((bytes_read = read(fd, buf, sizeof(buf))) > 0)
      archive_write_data(archive, buf, bytes_read);
    archive_write_finish_entry(archive);
    archive_read_free(ard);
    archive_entry_free(entry);
  }

  std::filesystem::current_path(old_path);
  archive_write_close(archive);
  archive_write_free(archive);
}
}
