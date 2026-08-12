/* Copyright (C) 2024 John Törnblom

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

/*
 * Modified for ProsperoPlayer in 2026:
 * - removed websrv-specific dependencies;
 * - uses the Prospero media launcher file reader;
 * - keeps the upstream BigApp transition and ELF replacement sequence.
 */

#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>

#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <ps5/kernel.h>

#include "standalone_fs.h"
#include "elfldr.h"
#include "hbldr.h"
#include "pt.h"


#define PSNOW_EBOOT "/system_ex/app/NPXS40106/eboot.bin"
#define HOST_TITLE_ID "PRSP10001"
#define FAKE_PATH "/system_ex/app/" HOST_TITLE_ID

#define IOVEC_ENTRY(x) {x ? x : 0, \
			x ? strlen(x)+1 : 0}
#define IOVEC_SIZE(x) (sizeof(x) / sizeof(struct iovec))


/*
 * Host param intentionally has NO contentId.
 * A fake contentId makes the shell license-check the title and often shows
 * "not supported" / lock on the Media row. Tile param (user/app) also omits it.
 *
 * Category MUST be 0 (Native Game), like upstream websrv hbldr:
 * media-category (65536) app processes are service-mediated on PS5
 * (VideoCore -> compositor) and are denied raw VideoOut —
 * sceVideoOutOpen fails with 0x80290009 (RESOURCE_BUSY).
 * Game-category processes own the display pipeline directly.
 * The dashboard tile keeps its own media param with the deeplink;
 * this host param is what the launched BigApp process runs under.
 */
static const char param_json[] = "{\n"
  "  \"applicationCategoryType\": 0,\n"
  "  \"attribute\": 1,\n"
  "  \"attribute2\": 0,\n"
  "  \"attribute3\": 4,\n"
  "  \"titleId\": \"" HOST_TITLE_ID "\",\n"
  "  \"localizedParameters\": {\n"
  "    \"defaultLanguage\": \"en-US\",\n"
  "    \"en-US\": {\n"
  "      \"titleName\": \"PS Play\"\n"
  "    }\n"
  "  }\n"
  "}\n";


typedef struct app_launch_ctx {
  uint32_t structsize;
  uint32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
} app_launch_ctx_t;


int sceUserServiceInitialize(void*);
int sceUserServiceGetForegroundUser(uint32_t *user_id);
int sceUserServiceTerminate(void);

int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
int sceSystemServiceLaunchApp(const char* title_id, char** argv,
			      app_launch_ctx_t* ctx);

int sceKernelGetAppState(int app_id, int*, int*);


static int
remount_system_ex(void) {
  struct iovec iov[] = {
    IOVEC_ENTRY("from"),      IOVEC_ENTRY("/dev/ssd0.system_ex"),
    IOVEC_ENTRY("fspath"),    IOVEC_ENTRY("/system_ex"),
    IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
    IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
    IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
    IOVEC_ENTRY("async"),     {NULL, 0},
    IOVEC_ENTRY("ignoreacl"), {NULL, 0},
  };

  if(nmount(iov, IOVEC_SIZE(iov), MNT_UPDATE)) {
    return -1;
  }

  return 0;
}


static int
write_all(int fd, const uint8_t* data, size_t size) {
  size_t offset = 0;

  while(offset < size) {
    ssize_t written = write(fd, data + offset, size - offset);
    if(written <= 0) {
      if(!written) {
        errno = ENOSPC;
      }
      return -1;
    }
    offset += (size_t)written;
  }
  return 0;
}

static int
regular_file_has_exact_size(const char* path, off_t expected_size) {
  struct stat info;

  return !lstat(path, &info) && S_ISREG(info.st_mode) &&
         !S_ISLNK(info.st_mode) && info.st_size == expected_size;
}

static int
write_file_replace(const char* path, const uint8_t* data, size_t size,
                   mode_t mode) {
  int fd;

  if((fd=open(path, O_CREAT|O_WRONLY|O_TRUNC, mode)) < 0) {
    return -1;
  }
  if(write_all(fd, data, size) || fsync(fd)) {
    int error = errno;
    close(fd);
    errno = error;
    return -1;
  }
  if(close(fd)) {
    return -1;
  }
  return chmod(path, mode);
}


static int
fakeapp_create_if_missing(void) {
  struct stat info;
  struct stat source_info;
  uint8_t* buf;
  size_t size;

  if(lstat(FAKE_PATH, &info)) {
    if(mkdir(FAKE_PATH, 0755) && errno != EEXIST) {
      return -1;
    }
  } else if(!S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode)) {
    errno = ENOTDIR;
    return -1;
  }

  if(lstat(FAKE_PATH "/sce_sys", &info)) {
    if(mkdir(FAKE_PATH "/sce_sys", 0755) && errno != EEXIST) {
      return -1;
    }
  } else if(!S_ISDIR(info.st_mode) || S_ISLNK(info.st_mode)) {
    errno = ENOTDIR;
    return -1;
  }

  /* Always rewrite: the param content (category/attributes) may change
     between builds while size stays identical. */
  if(write_file_replace(FAKE_PATH "/sce_sys/param.json",
                        (const uint8_t*)param_json,
                        sizeof(param_json)-1, 0644)) {
    return -1;
  }

  if(lstat(PSNOW_EBOOT, &source_info) || !S_ISREG(source_info.st_mode) ||
     S_ISLNK(source_info.st_mode) || source_info.st_size < 4096) {
    errno = ENOEXEC;
    return -1;
  }
  if(!regular_file_has_exact_size(FAKE_PATH "/eboot.bin",
                                  source_info.st_size)) {
    if(!(buf=fs_readfile(PSNOW_EBOOT, &size))) {
      return -1;
    }
    if(write_file_replace(FAKE_PATH "/eboot.bin", buf, size, 0755)) {
      free(buf);
      return -1;
    }
    free(buf);
  }
  return 0;
}


int
hbldr_prepare_host(void) {
  if(!fakeapp_create_if_missing()) {
    return 0;
  }
  if(remount_system_ex()) {
    perror("remount_system_ex");
    return -1;
  }
  if(fakeapp_create_if_missing()) {
    perror("fakeapp_create_if_missing");
    return -1;
  }
  return 0;
}


static void
hbldr_try_unlink(const char* path) {
  if(path)
    (void)unlink(path);
}


static void
hbldr_try_rmdir(const char* path) {
  if(path)
    (void)rmdir(path);
}


int
hbldr_remove_host(void) {
  /* Best-effort remount so system_ex is writable */
  (void)remount_system_ex();

  hbldr_try_unlink(FAKE_PATH "/sce_sys/param.json");
  hbldr_try_unlink(FAKE_PATH "/sce_sys/icon0.png");
  hbldr_try_rmdir(FAKE_PATH "/sce_sys");
  hbldr_try_unlink(FAKE_PATH "/eboot.bin");
  hbldr_try_rmdir(FAKE_PATH);

  /* Success if host directory is gone or was never present */
  {
    struct stat info;
    if(lstat(FAKE_PATH, &info) != 0)
      return 0;
  }
  return -1;
}


/**
 * Get the pid of a process with the given name.
 **/
static pid_t
find_pid(const char* name) {
  int mib[4] = {1, 14, 8, 0};
  pid_t mypid = getpid();
  pid_t pid = -1;
  size_t buf_size;
  uint8_t *buf;

  if(sysctl(mib, 4, 0, &buf_size, 0, 0)) {
    perror("sysctl");
    return -1;
  }

  if(!(buf=malloc(buf_size))) {
    perror("malloc");
    return -1;
  }

  if(sysctl(mib, 4, buf, &buf_size, 0, 0)) {
    perror("sysctl");
    return -1;
  }

  for(uint8_t *ptr=buf; ptr<(buf+buf_size);) {
    int ki_structsize = *(int*)ptr;
    pid_t ki_pid = *(pid_t*)&ptr[72];
    char *ki_tdname = (char*)&ptr[447];

    ptr += ki_structsize;
    if(!strcmp(name, ki_tdname) && mypid != ki_pid) {
      pid = ki_pid;
    }
  }

  free(buf);

  return pid;
}


/**
 *
 **/
static void*
bigapp_launch_thread(void* arg) {
  char** argv = (char**)arg;
  app_launch_ctx_t ctx = {0};
  int result;

  if(sceUserServiceGetForegroundUser(&ctx.user_id)) {
    perror("sceUserServiceGetForegroundUser");
    return 0;
  }

  result = sceSystemServiceLaunchApp(HOST_TITLE_ID, argv, &ctx);
  if(result) {
    fprintf(stderr, "sceSystemServiceLaunchApp(%s) failed: 0x%08x\n",
            HOST_TITLE_ID, result);
  }

  return 0;
}


/**
 *
 **/
static pid_t
bigapp_launch(char** argv) {
  struct kevent evt = {0};
  pid_t parent = -1;
  pid_t child = -1;
  pthread_t trd;
  int kq = -1;
  struct timespec timeout = {30, 0};

  if((parent=find_pid("SceSysCore.elf")) < 0) {
    perror("findpid");
    return -1;
  }

  if((kq=kqueue()) < 0) {
    perror("kqueue");
    return -1;
  }

  EV_SET(&evt, parent, EVFILT_PROC, EV_ADD | EV_ENABLE | EV_CLEAR,
	 NOTE_FORK | NOTE_EXEC | NOTE_TRACK, 0, 0);
  if(kevent(kq, &evt, 1, 0, 0, 0) < 0) {
    perror("kevent");
    close(kq);
    return -1;
  }

  if(pthread_create(&trd, 0, &bigapp_launch_thread, (void*)argv)) {
    perror("pthread_create");
    close(kq);
    return -1;
  }
  pthread_detach(trd);

  int event_count = kevent(kq, 0, 0, &evt, 1, &timeout);
  if(event_count < 0) {
    perror("kevent");
    close(kq);
    return -1;
  }
  if(!event_count) {
    errno = ETIMEDOUT;
    perror("BigApp launch event");
    close(kq);
    return -1;
  }

  close(kq);

  if(!(evt.fflags & NOTE_CHILD)) {
    return -1;
  }

  child = evt.ident;

  if(pt_attach(child)) {
    perror("pt_attach");
    return -1;
  }

  if(pt_follow_exec(child) < 0) {
    perror("pt_follow_exec");
    pt_detach(child, SIGKILL);
    return -1;
  }

  if(pt_continue(child, SIGCONT) < 0) {
    perror("pt_continue");
    pt_detach(child, SIGKILL);
    return -1;
  }

  if(pt_await_exec(child)) {
    perror("pt_await_exec");
    pt_detach(child, SIGKILL);
    return -1;
  }

  return child;
}


/**
 *
 **/
static int
bigapp_set_argv0(pid_t pid, const char* argv0) {
  intptr_t pos = pt_getargv(pid);
  intptr_t buf = 0;

  // allocate memory
  if((buf=pt_mmap(pid, 0, PAGE_SIZE, PROT_WRITE | PROT_READ,
		  MAP_ANONYMOUS | MAP_PRIVATE,
		  -1, 0)) == -1) {
    pt_perror(pid, "pt_mmap");
    return -1;
  }

  // copy string
  if(pt_copyin(pid, argv0, buf, strlen(argv0)+1)) {
    perror("pt_copyin");
    pt_munmap(pid, buf, PAGE_SIZE);
    return -1;
  }

  // copy pointer to string
  if(pt_setlong(pid, pos, buf)) {
    perror("pt_setlong");
    pt_munmap(pid, buf, PAGE_SIZE);
    return -1;
  }

  return 0;
}


/**
 *
 **/
static pid_t
bigapp_replace(pid_t pid, uint8_t* elf, const char* progname, int stdio,
	       const char* cwd, char** envp) {
  uint8_t int3instr = 0xcc;
  intptr_t brkpoint;
  uint8_t orginstr;

  // Let the kernel assign process parameters accessed via sceKernelGetProcParam()
  if(pt_syscall(pid, 599)) {
    puts("sys_dynlib_process_needed_and_relocate failed");
    //return -1;
  }

  // Allow libc to allocate arbitrary amount of memory.
  elfldr_set_heap_size(pid, -1);

  if(!(brkpoint=kernel_dynlib_entry_addr(pid, 0))) {
    puts("kernel_dynlib_entry_addr failed");
    return -1;
  }
  brkpoint += 58;// offset to invocation of main()

  if(kernel_mprotect(pid, brkpoint, PAGE_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC)) {
    puts("kernel_mprotect failed");
    pt_detach(pid, SIGKILL);
    return -1;
  }

  if(pt_copyout(pid, brkpoint, &orginstr, sizeof(orginstr))) {
    perror("pt_copyout");
    return -1;
  }
  if(pt_copyin(pid, &int3instr, brkpoint, sizeof(int3instr))) {
    perror("pt_copyin");
    return -1;
  }

  // Continue execution until we hit the breakpoint, then remove it.
  if(pt_continue(pid, SIGCONT)) {
    perror("pt_continue");
    return -1;
  }
  if(waitpid(pid, 0, 0) == -1) {
    perror("waitpid");
    return -1;
  }
  if(pt_copyin(pid, &orginstr, brkpoint, sizeof(orginstr))) {
    perror("pt_copyin");
    return -1;
  }

  bigapp_set_argv0(pid, progname);
  elfldr_set_procname(pid, basename(progname));
  elfldr_set_environ(pid, envp);
  elfldr_set_cwd(pid, cwd);
  elfldr_set_stdio(pid, stdio);

#if 0
  // invoke sys_set_butget(0)
  // This allow bigapp homebrew to operate on relative paths using
  // open, mkdir, etc.
  // TODO: this also causes syscore to crash when the bigapp terminates
  pt_syscall(pid, 0x23b, 0);
#endif
  
  // Execute the ELF
  if(elfldr_exec(pid, elf)) {
    return -1;
  }

  return pid;
}


static pid_t
hbldr_launch_elf(const char* cwd, const char* path, int stdio, char** argv,
                 char** envp, uint8_t* elf) {
  int app_id;
  int wait_attempt;
  pid_t pid;

  if(!cwd || !path || !elf) {
    return -1;
  }

  if(hbldr_prepare_host()) {
    return -1;
  }

  if((app_id=sceSystemServiceGetAppIdOfRunningBigApp()) > 0) {
    if(sceSystemServiceKillApp(app_id, -1, 0, 0)) {
      perror("sceSystemServiceKillApp");
      return -1;
    }

    for(wait_attempt=0;
        wait_attempt<30 && !sceKernelGetAppState(app_id, 0, 0);
        wait_attempt++) {
      printf("Waiting for App with id 0x%x to terminate\n", app_id);
      sleep(1);
    }
    if(wait_attempt == 30 && !sceKernelGetAppState(app_id, 0, 0)) {
      errno = ETIMEDOUT;
      perror("BigApp termination");
      return -1;
    }
  }

  if((pid=bigapp_launch(argv)) < 0) {
    return -1;
  }

  elfldr_raise_privileges(pid);

  if(bigapp_replace(pid, elf, path, stdio, cwd, envp) < 0) {
    pt_detach(pid, SIGKILL);
    pid = -1;
  }

  return pid;
}


pid_t
hbldr_launch_buffer(const char* cwd, const char* path, int stdio, char** argv,
                    char** envp, const uint8_t* elf) {
  return hbldr_launch_elf(cwd, path, stdio, argv, envp, (uint8_t*)elf);
}


pid_t
hbldr_launch(const char*cwd, const char* path, int stdio, char** argv,
	     char** envp) {
  char buf[PATH_MAX];
  uint8_t* elf;
  pid_t pid;

  if(!cwd || !path) {
    return -1;
  }

  if(path[0] == '/') {
    snprintf(buf, sizeof(buf), "%s", path);
  } else {
    snprintf(buf, sizeof(buf), "%s/%s", cwd, path);
  }

  if(!(elf=fs_readfile(buf, 0))) {
    return -1;
  }

  pid = hbldr_launch_elf(cwd, path, stdio, argv, envp, elf);
  free(elf);
  return pid;
}
