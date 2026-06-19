/*
 *
 * This is offered under a BSD-style license. This means you can use the code
 * for whatever you desire in any way you may want but you MUST NOT forget to
 * give me appropriate credits when spreading your work which is based on mine.
 * Something like "original implementation by Radek Podgorny" should be fine.
 *
 * License: BSD-style license
 * Copyright: Radek Podgorny <radek@podgorny.cz>,
 *            Bernd Schubert <bernd-schubert@gmx.de>
 */

#include <archive.h>
#include <sys/wait.h>
#include <sys/xattr.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <expected>
#include <filesystem>
#include <thread>

#include "include/belladonna.h"

extern "C" {
#include "diffgen.h"
#include <dirent.h>
#include <fcntl.h>
#include <fuse.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "debug.h"
#include "fuse.h"
#include "fuse_log.h"
#include "fuse_lowlevel.h"
#include "fuse_opt.h"
#include "opts.h"
#include "unionfs.h"
#include "usyslog.h"
}

static void* passthrough_init(struct fuse_conn_info* conn,
                              struct fuse_config* cfg) {
  (void)conn;
  cfg->kernel_cache = 1;

  return NULL;
}

static int passthrough_getattr(const char* path, struct stat* stbuf,
                               struct fuse_file_info* fi) {
  auto ret = lstat(path, stbuf);
  return ret;
}

static int passthrough_getxattr(const char* path, const char* key, char* buf,
                                size_t size) {
  return lgetxattr(path, key, buf, size);
}
static int passthrough_listxattr(const char* path, char* list, size_t size) {
  return llistxattr(path, list, size) == -1 ? -errno : 0;
}

// read only so no xattr fun :(
static int passthrough_removexattr(const char*, const char*) { return 0; }
static int passthrough_setxattr(const char*, const char*, const char*, size_t,
                                int) {
  return 0;
}

static int passthrough_readdir(const char* path, void* buf,
                               fuse_fill_dir_t filler, off_t offset,
                               struct fuse_file_info* fi,
                               enum fuse_readdir_flags flags) {
  DIR* dp;
  struct dirent* de;

  (void)offset;
  (void)fi;
  (void)flags;

  dp = opendir(path);
  if (dp == nullptr) return -errno;

  while ((de = readdir(dp)) != nullptr) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino = de->d_ino;
    st.st_mode = de->d_type << 12;

    filler(buf, de->d_name, &st, 0, FUSE_FILL_DIR_PLUS);
  }

  closedir(dp);
  return 0;
}

static int passthrough_open(const char* path, struct fuse_file_info* fi) {
  int ret = open(path, fi->flags);
  fi->fh = (uint64_t)ret;

  return 0;
}
static int passthrough_release(const char* path, struct fuse_file_info* fi) {
  int fd = (int)fi->fh;

  return close(fd);
}

static int passthrough_readlink(const char* path, char* buf, size_t size) {
  ssize_t link_len = readlink(path, buf, size);
  auto status = link_len != -1 ? 0 : -1;
  if (status == 0) {
    buf[std::min((size_t)link_len, (size_t)size - 1)] = 0;
  }

  return status;
}

static int passthrough_read(const char* path, char* buf, size_t size,
                            off_t offset, struct fuse_file_info* fi) {
  int fd = (int)fi->fh;

  return (int)pread(fd, buf, size, offset);
}

static const struct fuse_operations passthrough_oper = {
    .getattr = passthrough_getattr,
    .readlink = passthrough_readlink,
    .open = passthrough_open,
    .read = passthrough_read,
    .release = passthrough_release,
    .setxattr = passthrough_setxattr,
    .getxattr = passthrough_getxattr,
    .listxattr = passthrough_listxattr,
    .removexattr = passthrough_removexattr,
    .readdir = passthrough_readdir,
    .init = passthrough_init,
};

std::atomic<bool> root_ready = false;
std::atomic<fuse*> root_fuse = nullptr;
std::atomic<bool> root_done = false;

void root_fuse_main(char* dir) {
  char* argv[] = {"passthrough", nullptr};
  struct fuse_args args = FUSE_ARGS_INIT(1, argv);

  struct fuse* fuse =
      fuse_new(&args, &passthrough_oper, sizeof(passthrough_oper), nullptr);
  fuse_mount(fuse, dir);

  root_ready = true;
  root_ready.notify_all();
  root_fuse = fuse;

  struct fuse_loop_config* config = fuse_loop_cfg_create();
  fuse_loop_mt(fuse, config);
  fuse_loop_cfg_destroy(config);

  fuse_unmount(fuse);
  fuse_destroy(fuse);

  fuse_opt_free_args(&args);

  umount(dir);
  remove(dir);
  root_done = true;
  root_done.notify_all();

  return;
}

std::atomic<bool> unionfs_ready = false;
std::atomic<fuse*> unionfs_fuse = nullptr;
std::atomic<bool> unionfs_done = false;

static struct fuse_opt unionfs_opts[] = {
    FUSE_OPT_KEY("chroot=%s,", KEY_CHROOT),
    FUSE_OPT_KEY("cow", KEY_COW),
    FUSE_OPT_KEY("preserve_branch", KEY_PRESERVE_BRANCH),
    FUSE_OPT_KEY("debug_file=%s", KEY_DEBUG_FILE),
    FUSE_OPT_KEY("dirs=%s", KEY_DIRS),
    FUSE_OPT_KEY("--help", KEY_HELP),
    FUSE_OPT_KEY("-h", KEY_HELP),
    FUSE_OPT_KEY("hide_meta_dir", KEY_HIDE_METADIR),
    FUSE_OPT_KEY("hide_meta_files", KEY_HIDE_META_FILES),
    FUSE_OPT_KEY("max_files=%s", KEY_MAX_FILES),
    FUSE_OPT_KEY("noinitgroups", KEY_NOINITGROUPS),
    FUSE_OPT_KEY("relaxed_permissions", KEY_RELAXED_PERMISSIONS),
    FUSE_OPT_KEY("statfs_omit_ro", KEY_STATFS_OMIT_RO),
    FUSE_OPT_KEY("direct_io", KEY_DIRECT_IO),
    FUSE_OPT_KEY("--version", KEY_VERSION),
    FUSE_OPT_KEY("-V", KEY_VERSION),
    FUSE_OPT_END};

void unionfs_main(char* union_jail, char* branches, char* mount_dir,
                  char* changes_dir, char* base_dir) {
  int argc_internal = 3;
  char* argv_internal[] = {"unionfs", union_jail, branches, nullptr};

  struct fuse_args args = FUSE_ARGS_INIT(argc_internal, argv_internal);

  uopt_init();

  if (fuse_opt_parse(&args, nullptr, unionfs_opts, unionfs_opt_proc) == -1)
    return;

  if (uopt.debug) debug_init();

  if (!uopt.doexit) {
    if (uopt.nbranches == 0) {
      return;
    }
  }

  uid_t uid = getuid();
  gid_t gid = getgid();
  bool default_permissions = true;

  if (uid != 0 && gid != 0 && uopt.relaxed_permissions) {
    default_permissions = false;
  } else if (uopt.relaxed_permissions) {
    // protect the user of a very critical security issue
    fprintf(stderr, "Relaxed permissions disallowed for root!");
    return;
  }

  if (default_permissions) {
    if (fuse_opt_add_arg(&args, "-odefault_permissions")) {
      fprintf(stderr,
                   "Severe failure, can't enable permssion checks, aborting!");
      return;
    }
  }
  unionfs_post_opts();

#ifdef FUSE_CAP_BIG_WRITES
  /* libfuse > 0.8 supports large IO, also for reads, to increase performance
   * We support any IO sizes, so lets enable that option */
  if (fuse_opt_add_arg(&args, "-obig_writes")) {
    fprintf(stderr, "Failed to enable big writes!\n");
    exit(1);
  }
#endif

  umask(0);

  struct fuse* fuse =
      fuse_new(&args, &unionfs_oper, sizeof(unionfs_oper), nullptr);
  fuse_mount(fuse, mount_dir);

  unionfs_ready = true;
  unionfs_ready.notify_all();

  unionfs_fuse = fuse;

  struct fuse_loop_config* config = fuse_loop_cfg_create();
  fuse_loop_mt(fuse, config);
  fuse_loop_cfg_destroy(config);

  fuse_opt_free_args(&args);

  fuse_unmount(fuse);
  fuse_destroy(fuse);

  umount(mount_dir);
  umount(changes_dir);

  remove(changes_dir);
  remove(mount_dir);

  remove(base_dir);

  unionfs_done = true;
  unionfs_done.notify_all();
}

std::expected<BelladonnaState*, std::string>
BelladonnaState::belladonna_create_sandbox() {
  auto* state = new BelladonnaState;
  uid_t uid = getuid();
  gid_t gid = getgid();

  if (uid != 0 && gid != 0) {
    return std::unexpected("error: cannot sandbox without root");
  }

  if (unshare(CLONE_NEWNS) != 0) {
    return std::unexpected(strerror(errno));
  }
  mount(nullptr, "/", nullptr, MS_SLAVE | MS_REC, nullptr);

  time_t epoch = time(nullptr);

  state->base_dir = (char*)malloc(1024);
  snprintf(state->base_dir, 1024, "/tmp/foxisolate.%li", epoch);

  state->changes_dir = (char*)malloc(1024);
  snprintf(state->changes_dir, 1024, "/tmp/foxisolate.%li/changes", epoch);

  state->mount_dir = (char*)malloc(1024);
  snprintf(state->mount_dir, 1024, "/tmp/foxisolate.%li/mount", epoch);

  state->dev_dir = (char*)malloc(1024);
  snprintf(state->dev_dir, 1024, "/tmp/foxisolate.%li/mount/dev", epoch);

  state->sys_dir = (char*)malloc(1024);
  snprintf(state->sys_dir, 1024, "/tmp/foxisolate.%li/mount/sys", epoch);

  state->proc_dir = (char*)malloc(1024);
  snprintf(state->proc_dir, 1024, "/tmp/foxisolate.%li/mount/proc", epoch);

  state->root_dir = (char*)malloc(1024);
  snprintf(state->root_dir, 1024, "/tmp/foxisolate.%li/root", epoch);
  mkdir(state->base_dir, 0);
  mkdir(state->root_dir, 0);

  state->root_thread = std::thread(root_fuse_main, state->root_dir);

  while (!root_ready) {
    root_ready.wait(false);
  }

  mkdir(state->changes_dir, 0);
  mkdir(state->mount_dir, 0);

  mount("foxisolate", state->changes_dir, "tempfs", 0, nullptr);

  state->union_jail = (char*)malloc(1024);
  snprintf(state->union_jail, 1024, "-ocow");

  state->branches = (char*)malloc(1024);
  snprintf(state->branches, 1024, "%s=RW:%s=RO", state->changes_dir,
           state->root_dir);

  state->unionfs_thread =
      std::thread(unionfs_main, state->union_jail, state->branches,
                  state->mount_dir, state->changes_dir, state->base_dir);

  while (!unionfs_ready) {
    unionfs_ready.wait(false);
  }

  mount("/dev", state->dev_dir, nullptr, MS_BIND | MS_REC, nullptr);
  mount("/proc", state->proc_dir, nullptr, MS_BIND | MS_REC, nullptr);
  mount("/sys", state->sys_dir, nullptr, MS_BIND | MS_REC, nullptr);
  return state;
}

int BelladonnaState::fork_into() {
  auto child = fork();
  if (child == 0) {
    auto current_path = std::filesystem::current_path();
    if (chroot(mount_dir) != 0) {
      fprintf(stderr, "error: %s\n", strerror(errno));
      exit(0);
    }
    if (chdir(current_path.c_str()) != 0) {
      fprintf(stderr, "error: %s\n", strerror(errno));
      exit(0);
    }
  }
  return child;
}

void BelladonnaState::start_shell() {
  auto child = fork();
  if (child == 0) {
    auto current_path = std::filesystem::current_path();
    if (chroot(mount_dir) != 0) {
      fprintf(stderr, "error: %s\n", strerror(errno));
      exit(0);
    }
    if (chdir(current_path.c_str()) != 0) {
      fprintf(stderr, "error: %s\n", strerror(errno));
      exit(0);
    }

    char* shell = "/bin/bash";

    std::array<char*, 2> argv_internal = {shell, nullptr};

    execv(shell, argv_internal.data());

    exit(0);
  }
  waitpid(child, nullptr, 0);
}

BelladonnaState::~BelladonnaState() {
  fuse_session_exit(fuse_get_session(unionfs_fuse));
  fuse_session_exit(fuse_get_session(root_fuse.load()));

  while (!root_done) {
    root_done.wait(false);
  }
  while (!unionfs_done) {
    unionfs_done.wait(false);
  }
  unionfs_thread.join();
  root_thread.join();

  diffgen(changes_dir, "/");

  umount(dev_dir);
  umount(sys_dir);
  umount(proc_dir);

  free((void*)changes_dir);
  free((void*)mount_dir);
  free((void*)dev_dir);
  free((void*)proc_dir);
  free((void*)root_dir);
  free((void*)base_dir);
  free((void*)sys_dir);

  free((void*)branches);
  free((void*)union_jail);
}
