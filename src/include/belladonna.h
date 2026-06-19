#pragma once

#include <string>
#include <thread>

class BelladonnaState {
 private:
  char* base_dir;
  char* changes_dir;
  char* mount_dir;
  char* dev_dir;
  char* sys_dir;
  char* proc_dir;
  char* root_dir;
  char* branches;
  char* union_jail;
  std::thread root_thread;
  std::thread unionfs_thread;

 public:
  static std::expected<BelladonnaState*, std::string>
  belladonna_create_sandbox();

  int fork_into();

  void start_shell();

  ~BelladonnaState();
};
