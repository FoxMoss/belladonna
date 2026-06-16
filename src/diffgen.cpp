
#include "diffgen.h"

#include <stdio.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

extern "C" {
void diffgen(const char* changed_dir, const char* truth_dir) {
  std::filesystem::recursive_directory_iterator iter(changed_dir);

  for (auto file : iter) {
    if (!file.is_regular_file()) {
      continue;
    }
    std::string real_file =
        file.path().string().substr(strlen(changed_dir), -1);
    if (!std::filesystem::exists(real_file)) {
      real_file = "/dev/null";
    }

    const char* argv[] = {"diff", "--color", real_file.c_str(),
                          file.path().c_str(), NULL};
    printf("\ndiff a%s b%s\n", real_file.c_str(), real_file.c_str());
    execv("/usr/bin/diff", (char* const*)argv);
  }
}
}
