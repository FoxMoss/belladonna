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

#include "diffgen.h"
#define _GNU_SOURCE
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
#include "fuse_log.h"
#include "fuse_opt.h"
#include "opts.h"
#include "unionfs.h"
#include "usyslog.h"

static void* passthrough_init(struct fuse_conn_info* conn,
                              struct fuse_config* cfg) {
  (void)conn;
  cfg->kernel_cache = 1;

  /* Test setting flags the old way */
  fuse_set_feature_flag(conn, FUSE_CAP_ASYNC_READ);
  fuse_unset_feature_flag(conn, FUSE_CAP_ASYNC_READ);

  return NULL;
}

static int passthrough_getattr(const char* path, struct stat* stbuf,
                               struct fuse_file_info* fi) {
  return stat(path, stbuf);
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
  if (dp == NULL) return -errno;

  while ((de = readdir(dp)) != NULL) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino = de->d_ino;
    st.st_mode = de->d_type << 12;

    filler(buf, de->d_name, &st, 0, FUSE_FILL_DIR_DEFAULTS);
  }

  closedir(dp);
  return 0;
}

static int passthrough_open(const char* path, struct fuse_file_info* fi) {
  int ret = open(path, fi->flags);
  fi->fh = (uint64_t)ret;

  return 0;
}

static int passthrough_read(const char* path, char* buf, size_t size,
                            off_t offset, struct fuse_file_info* fi) {
  int fd = (int)fi->fh;

  return pread(fd, buf, size, offset);
}

static const struct fuse_operations passthrough_oper = {
    .init = passthrough_init,
    .getattr = passthrough_getattr,
    .readdir = passthrough_readdir,
    .open = passthrough_open,
    .read = passthrough_read,
};

int start_root_passthrough(char* dir) {
  if (fork() != 0) {
    return 0;
  }
  int ret;
  char* argv[] = {"passthrough", "-s", "-f", dir, NULL};
  struct fuse_args args = FUSE_ARGS_INIT(4, argv);

  struct fuse_opt option_spec[] = {FUSE_OPT_END};

  ret = fuse_main(args.argc, args.argv, &passthrough_oper, NULL);
  fuse_opt_free_args(&args);

  umount(dir);
  remove(dir);

  exit(0);
}

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

int main(int argc, char* argv[]) {
  if (unshare(CLONE_NEWNS) != 0) {
    fprintf(stderr, "error: %s\n", strerror(errno));
    return 1;
  }

  time_t epoch = time(NULL);

  char* base_dir = (char*)malloc(1024);
  snprintf(base_dir, 1024, "/tmp/foxisolate.%li", epoch);

  char* changes_dir = (char*)malloc(1024);
  snprintf(changes_dir, 1024, "/tmp/foxisolate.%li/changes", epoch);

  char* mount_dir = (char*)malloc(1024);
  snprintf(mount_dir, 1024, "/tmp/foxisolate.%li/mount", epoch);

  char* dev_dir = (char*)malloc(1024);
  snprintf(dev_dir, 1024, "/tmp/foxisolate.%li/mount/dev", epoch);

  char* proc_dir = (char*)malloc(1024);
  snprintf(proc_dir, 1024, "/tmp/foxisolate.%li/mount/proc", epoch);

  char* root_dir = (char*)malloc(1024);
  snprintf(root_dir, 1024, "/tmp/foxisolate.%li/root", epoch);

  mkdir(base_dir, 0);
  mkdir(root_dir, 0);

  start_root_passthrough(root_dir);

  mkdir(changes_dir, 0);
  mkdir(mount_dir, 0);

  mount("foxisolate", changes_dir, "tempfs", 0, NULL);

  char* union_jail = malloc(1024);
  snprintf(union_jail, 1024, "-ocow");

  char* branches = malloc(1024);
  snprintf(branches, 1024, "%s=RW:%s=RO", changes_dir, root_dir);

  int argc_internal = 6;
  char* argv_internal[] = {"unionfs", "-s",      "-f", union_jail,
                           branches,  mount_dir, NULL};

  printf("%s\n", mount_dir);

  struct fuse_args args = FUSE_ARGS_INIT(argc_internal, argv_internal);

  // Initing syslog here is a problem because the log thread ends up exiting in
  // non foreground mode (see
  // https://github.com/rpodgorny/unionfs-fuse/issues/122) init_syslog();
  uopt_init();

  if (fuse_opt_parse(&args, NULL, unionfs_opts, unionfs_opt_proc) == -1)
    RETURN(1);

  if (uopt.debug) debug_init();

  if (!uopt.doexit) {
    if (uopt.nbranches == 0) {
      printf("You need to specify at least one branch!\n");
      RETURN(1);
    }
  }

  // enable fuse permission checks, we need to set this, even we we are
  // not root, since we don't have our own access() function
  uid_t uid = getuid();
  gid_t gid = getgid();
  bool default_permissions = true;

  if (uid != 0 && gid != 0 && uopt.relaxed_permissions) {
    default_permissions = false;
  } else if (uopt.relaxed_permissions) {
    // protect the user of a very critical security issue
    fprintf(stderr, "Relaxed permissions disallowed for root!\n");
    exit(1);
  }

  if (default_permissions) {
    if (fuse_opt_add_arg(&args, "-odefault_permissions")) {
      fprintf(stderr,
              "Severe failure, can't enable permssion checks, aborting!\n");
      exit(1);
    }
  }
  unionfs_post_opts();

  uopt.dev = dev_dir;
  uopt.proc = proc_dir;

#ifdef FUSE_CAP_BIG_WRITES
  /* libfuse > 0.8 supports large IO, also for reads, to increase performance
   * We support any IO sizes, so lets enable that option */
  if (fuse_opt_add_arg(&args, "-obig_writes")) {
    fprintf(stderr, "Failed to enable big writes!\n");
    exit(1);
  }
#endif

  if (fork() == 0) {
    sleep(1);

    mount("/dev", uopt.dev, NULL, MS_BIND, NULL);
    mount("/proc", uopt.proc, NULL, MS_BIND, NULL);

    return 0;
  }

  umask(0);
  int res = fuse_main(args.argc, args.argv, &unionfs_oper, NULL);

  diffgen(changes_dir, "/");

  umount(uopt.dev);
  umount(uopt.proc);

  umount(mount_dir);
  umount(changes_dir);

  remove(changes_dir);
  remove(mount_dir);

  remove(base_dir);
  return 0;
}
