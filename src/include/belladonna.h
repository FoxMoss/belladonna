#pragma once

#include <expected>
#include <string>
#include <thread>

struct fuse;

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

  std::atomic<bool> root_ready = false;
  std::atomic<fuse*> root_fuse = nullptr;
  std::atomic<bool> root_done = false;

  std::atomic<bool> unionfs_ready = false;
  std::atomic<fuse*> unionfs_fuse = nullptr;
  std::atomic<bool> unionfs_done = false;

 public:
  static std::expected<BelladonnaState*, std::string>
  belladonna_create_sandbox();

  int fork_into();

  void start_shell();

  ~BelladonnaState();

 private:
  static void root_fuse_main(BelladonnaState *state, char* dir);
  static void unionfs_main(BelladonnaState *state, char* union_jail, char* branches, char* mount_dir,
                  char* changes_dir, char* base_dir);


};
