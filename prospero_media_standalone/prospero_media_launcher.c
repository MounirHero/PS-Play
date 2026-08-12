/*
 * ProsperoPlayer — Media home launcher (resident)
 *
 * Technique (industry-standard PS5 homebrew Media BigApp path):
 *  - Register a Media-category title (applicationCategoryType 65536)
 *  - System host under /system_ex/app/<titleId> uses a cloned system eboot
 *    (NPXS40106) as the BigApp shell; the real player is NOT that eboot
 *  - AppInstUtil with elevated authid before title registration
 *  - Tile deeplink hits a loopback-only HTTP service on this process
 *  - /launch maps the player ELF into the BigApp via hbldr (websrv core)
 *  - Successful handoff returns HTTP 204 with empty body
 *  - /uninstall fully removes the Media tile + host (XMB Delete often
 *    unavailable for Media BigApp hosts; this is the supported removal path)
 *
 * Product identity is ProsperoPlayer only (title PRSP10001, port 9055).
 * Loader modules in core/ are John Törnblom websrv-derived (GPL-3.0+).
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <ps5/kernel.h>
#include <ps5/payload.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "core/hbldr.h"
#include "core/standalone_fs.h"

#define PP_VERSION "1.5"
#define PP_TITLE_ID "PRSP10001"
#define PP_SERVICE_PORT 9055
#define PP_APPINST_AUTHID UINT64_C(0x4801000000000013)
#define PP_APP_ROOT "/user/app"
#define PP_APP_DIR PP_APP_ROOT "/" PP_TITLE_ID
#define PP_RUNTIME_DIR "/data/homebrew/ProsperoPlayer"
#define PP_PLAYER_PATH PP_RUNTIME_DIR "/eboot.elf"
#define PP_LOG_DIR "/data/prosperoplayer"
#define PP_LOG_PATH PP_LOG_DIR "/media_launcher.log"
#define PP_PLAYER_LOG PP_LOG_DIR "/player-stdio.log"

/* Earlier Prospero experiment IDs — remove so only PRSP10001 remains */
static const char *const PP_LEGACY_TITLES[] = {
    "PRSP00001",
    "PSMC00002", /* old mis-registered Media id from early tests */
    NULL,
};

#define INCASSET(name, file)                                                   \
  __asm__(".section .rodata\n"                                                 \
          ".global " #name "\n" #name ":\n"                                    \
          ".incbin \"" file "\"\n" #name                                       \
          "_end:\n"                                                            \
          ".global " #name "_size\n" #name                                     \
          "_size:\n"                                                           \
          ".quad " #name "_end - " #name "\n");                                \
  extern const uint8_t name[];                                                 \
  extern const unsigned long name##_size

INCASSET(pp_player_elf, "assets/ProsperoPlayer.elf");
INCASSET(pp_tile_param, "assets/param.json");
INCASSET(pp_icon0, "assets/icon0.png");

typedef int (*pp_install_title_dir_fn)(const char *, const char *, void *);

int sceUserServiceInitialize(void *);
int sceUserServiceTerminate(void);
int sceAppInstUtilInitialize(void);
int sceAppInstUtilTerminate(void);
int sceAppInstUtilAppInstallAll(void *);
int sceAppInstUtilAppUnInstall(const char *, void *, void *);
int sceKernelSendNotificationRequest(int, void *, size_t, int);
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);

int sceKernelGetAppState(int app_id, int *a, int *b) __attribute__((weak));
int sceKernelGetAppState(int app_id, int *a, int *b) {
  (void)app_id;
  (void)a;
  (void)b;
  return -1;
}

typedef struct {
  char reserved[45];
  char message[3075];
} pp_notify_t;

static void pp_log(const char *fmt, ...) {
  char body[1024];
  char line[1280];
  va_list ap;
  int n, fd;

  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt ? fmt : "", ap);
  va_end(ap);
  n = snprintf(line, sizeof(line), "%ld %s\n", (long)time(NULL), body);
  if (n <= 0)
    return;
  if ((size_t)n >= sizeof(line))
    n = (int)sizeof(line) - 1;
  fputs(line, stdout);
  fflush(stdout);
  (void)mkdir("/data", 0755);
  (void)mkdir(PP_LOG_DIR, 0755);
  fd = open(PP_LOG_PATH, O_WRONLY | O_CREAT | O_APPEND, 0600);
  if (fd >= 0) {
    (void)write(fd, line, (size_t)n);
    close(fd);
  }
}

uint8_t *fs_readfile(const char *path, size_t *size) {
  struct stat st;
  uint8_t *buf;
  size_t off = 0;
  int fd;
  ssize_t n;

  if (!path || stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
    return NULL;
  buf = (uint8_t *)malloc((size_t)st.st_size);
  if (!buf)
    return NULL;
  fd = open(path, O_RDONLY);
  if (fd < 0) {
    free(buf);
    return NULL;
  }
  while (off < (size_t)st.st_size) {
    n = read(fd, buf + off, (size_t)st.st_size - off);
    if (n <= 0) {
      close(fd);
      free(buf);
      return NULL;
    }
    off += (size_t)n;
  }
  close(fd);
  if (size)
    *size = (size_t)st.st_size;
  return buf;
}

static int pp_mkdir(const char *path) {
  return (mkdir(path, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static int pp_write_file(const char *path, const uint8_t *data, size_t size,
                         mode_t mode) {
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, mode);
  size_t off = 0;
  if (fd < 0)
    return -1;
  while (off < size) {
    ssize_t n = write(fd, data + off, size - off);
    if (n <= 0) {
      close(fd);
      return -1;
    }
    off += (size_t)n;
  }
  if (fsync(fd) != 0) {
    close(fd);
    return -1;
  }
  close(fd);
  return chmod(path, mode);
}

static void pp_notify(const char *msg) {
  pp_notify_t req;
  int rc;

  memset(&req, 0, sizeof(req));
  snprintf(req.message, sizeof(req.message), "%s", msg ? msg : "");
  rc = sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
  pp_log("notify rc=0x%08x msg=%s", (unsigned)rc, msg ? msg : "");
}

/* Autoload often runs before the shell is ready — delay + second toast. */
static void pp_notify_ready(void) {
  char line[96];
  snprintf(line, sizeof(line), "PS Play %s ready\nOpen from Media",
           PP_VERSION);
  pp_notify(line);
  usleep(1500000);
  snprintf(line, sizeof(line), "PS Play %s ready in Media", PP_VERSION);
  pp_notify(line);
}

static int pp_install_runtime(void) {
  if (pp_mkdir("/data") || pp_mkdir("/data/homebrew") ||
      pp_mkdir(PP_RUNTIME_DIR) || pp_mkdir(PP_RUNTIME_DIR "/sce_sys") ||
      pp_mkdir(PP_LOG_DIR)) {
    pp_log("runtime mkdir failed errno=%d", errno);
    return -1;
  }
  if (pp_write_file(PP_PLAYER_PATH, pp_player_elf, pp_player_elf_size, 0755) !=
      0) {
    pp_log("write player failed errno=%d", errno);
    return -1;
  }
  if (pp_write_file(PP_RUNTIME_DIR "/sce_sys/icon0.png", pp_icon0, pp_icon0_size,
                    0644) != 0 ||
      pp_write_file(PP_RUNTIME_DIR "/sce_sys/param.json", pp_tile_param,
                    pp_tile_param_size, 0644) != 0) {
    pp_log("write runtime sce_sys failed");
    return -1;
  }
  pp_log("runtime ready %s (%lu bytes)", PP_PLAYER_PATH,
         (unsigned long)pp_player_elf_size);
  return 0;
}

static int pp_with_appinst_authid(int (*fn)(void)) {
  const pid_t pid = getpid();
  const uint64_t orig = kernel_get_ucred_authid(pid);
  int rc;

  if (kernel_set_ucred_authid(pid, PP_APPINST_AUTHID) != 0) {
    pp_log("authid raise failed");
    pp_notify("PS Play authid raise FAILED\nkernel r/w unavailable?");
    return -1;
  }
  rc = fn();
  if (orig != 0)
    (void)kernel_set_ucred_authid(pid, orig);
  return rc;
}

static int pp_register_media_tile(void) {
  char sce[256], param[320], icon[320];
  uint32_t handle = 0;
  pp_install_title_dir_fn install_dir = NULL;
  int title_rc = -1, all_rc = -1, i;

  if (sceAppInstUtilInitialize() != 0) {
    pp_log("AppInstUtilInitialize failed");
    pp_notify("PS Play AppInstUtil init FAILED");
    return -1;
  }

  /* Drop experimental title IDs so only Prospero PRSP10001 remains */
  for (i = 0; PP_LEGACY_TITLES[i]; i++) {
    (void)sceAppInstUtilAppUnInstall(PP_LEGACY_TITLES[i], NULL, NULL);
    usleep(150000);
  }

  snprintf(sce, sizeof(sce), "%s/sce_sys", PP_APP_DIR);
  snprintf(param, sizeof(param), "%s/param.json", sce);
  snprintf(icon, sizeof(icon), "%s/icon0.png", sce);

  if (pp_mkdir(PP_APP_ROOT) || pp_mkdir(PP_APP_DIR) || pp_mkdir(sce)) {
    pp_log("mkdir tile dirs failed");
    (void)sceAppInstUtilTerminate();
    return -1;
  }

  /*
   * Tile metadata: Media category + attributes + deeplink only.
   * Do not put contentId on the dashboard tile (triggers lock / unsupported).
   * Host param under system_ex carries contentId (see core/hbldr.c).
   */
  if (pp_write_file(param, pp_tile_param, pp_tile_param_size, 0644) != 0 ||
      pp_write_file(icon, pp_icon0, pp_icon0_size, 0644) != 0) {
    pp_log("write tile assets failed");
    (void)sceAppInstUtilTerminate();
    return -1;
  }

  if (kernel_dynlib_handle(-1, "libSceAppInstUtil.sprx", &handle) == 0) {
    install_dir = (pp_install_title_dir_fn)kernel_dynlib_resolve(
        -1, handle, "Wudg3Xe3heE");
  }
  if (install_dir)
    title_rc = install_dir(PP_TITLE_ID, PP_APP_ROOT "/", NULL);
  if (title_rc != 0)
    all_rc = sceAppInstUtilAppInstallAll(NULL);

  pp_log("tile register title_dir=0x%08x install_all=0x%08x id=%s", title_rc,
         all_rc, PP_TITLE_ID);
  if (title_rc != 0 && all_rc != 0) {
    char msg[160];
    snprintf(msg, sizeof(msg),
             "PS Play tile register rc\ndir=0x%08x all=0x%08x",
             (unsigned)title_rc, (unsigned)all_rc);
    pp_notify(msg);
  }
  (void)sceAppInstUtilTerminate();
  return (title_rc == 0 || all_rc == 0) ? 0 : -1;
}

static int pp_bind_loopback_once(void) {
  struct sockaddr_in addr;
  int fd, on = 1;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(PP_SERVICE_PORT);
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/* Ask any previous resident launcher on this port to exit, then bind. */
static int pp_request_peer_shutdown(void) {
  struct sockaddr_in addr;
  char req[] = "GET /shutdown HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n";
  char resp[128];
  int fd;
  ssize_t n;

  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(PP_SERVICE_PORT);
  addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  (void)send(fd, req, sizeof(req) - 1, 0);
  n = recv(fd, resp, sizeof(resp) - 1, 0);
  if (n > 0)
    resp[n] = 0;
  close(fd);
  pp_log("peer shutdown requested");
  return 0;
}

static int pp_bind_loopback(void) {
  int fd = pp_bind_loopback_once();
  int attempt;

  if (fd >= 0)
    return fd;

  pp_log("port %d busy — requesting prior launcher shutdown", PP_SERVICE_PORT);
  (void)pp_request_peer_shutdown();
  for (attempt = 0; attempt < 20; attempt++) {
    usleep(100000);
    fd = pp_bind_loopback_once();
    if (fd >= 0) {
      pp_log("loopback bind ok after takeover attempt=%d", attempt + 1);
      return fd;
    }
  }
  return -1;
}

/*
 * Deeplink landing page shown by the shell webview on tile open.
 * Dark page that tries to close itself instantly; the player has
 * already been kicked off asynchronously at this point, so the
 * webview disappears as soon as the BigApp takes the display.
 */
static const char pp_launch_html[] =
    "<!doctype html><html><head><meta charset=utf-8><title>PS Play</title>"
    "<style>html,body{margin:0;background:#0a0b0e;height:100%;overflow:hidden}"
    "p{color:#555;font:14px sans-serif;text-align:center;margin-top:45%}</style>"
    "</head><body><p>PS Play</p>"
    "<script>try{window.close()}catch(e){};document.body.innerHTML=''</script>"
    "</body></html>";

static void pp_http_reply_html(int cfd, int status, const char *body) {
  char hdr[384];
  size_t blen = body ? strlen(body) : 0;
  int n = snprintf(hdr, sizeof(hdr),
                   "HTTP/1.1 %d OK\r\n"
                   "Content-Type: text/html; charset=utf-8\r\n"
                   "Content-Length: %zu\r\n"
                   "Cache-Control: no-store\r\n"
                   "Connection: close\r\n\r\n",
                   status, blen);
  if (n > 0)
    (void)send(cfd, hdr, (size_t)n, 0);
  if (blen)
    (void)send(cfd, body, blen, 0);
}

static void pp_http_reply(int cfd, int status, const char *body) {
  char hdr[384];
  size_t blen = body ? strlen(body) : 0;
  const char *reason =
      status == 204 ? "No Content" : status == 500 ? "Error" : "OK";
  int n = snprintf(hdr, sizeof(hdr),
                   "HTTP/1.1 %d %s\r\n"
                   "Content-Type: application/octet-stream\r\n"
                   "Content-Length: %zu\r\n"
                   "Cache-Control: no-store\r\n"
                   "Connection: close\r\n\r\n",
                   status, reason, blen);
  if (n > 0)
    (void)send(cfd, hdr, (size_t)n, 0);
  if (blen)
    (void)send(cfd, body, blen, 0);
}

static int pp_launch_player(void) {
  char *argv[] = {NULL};
  char *envp[] = {NULL};
  int stdio = open(PP_PLAYER_LOG, O_WRONLY | O_CREAT | O_APPEND, 0600);
  struct stat st;
  int fd;
  uint8_t *map;
  pid_t pid;

  if (pp_install_runtime() != 0) {
    if (stdio >= 0)
      close(stdio);
    return -1;
  }

  fd = open(PP_PLAYER_PATH, O_RDONLY);
  if (fd < 0 || fstat(fd, &st) != 0 || (size_t)st.st_size != pp_player_elf_size) {
    pp_log("player size mismatch errno=%d", errno);
    if (fd >= 0)
      close(fd);
    if (stdio >= 0)
      close(stdio);
    return -1;
  }
  map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (map == MAP_FAILED || map[0] != 0x7f) {
    pp_log("mmap player failed");
    if (map != MAP_FAILED)
      munmap(map, (size_t)st.st_size);
    if (stdio >= 0)
      close(stdio);
    return -1;
  }

  pp_log("hbldr_launch begin");
  pid = hbldr_launch_buffer(PP_RUNTIME_DIR, PP_PLAYER_PATH, stdio, argv, envp,
                            map);
  pp_log("hbldr_launch pid=%ld", (long)pid);
  munmap(map, (size_t)st.st_size);
  if (stdio >= 0)
    close(stdio);
  return pid > 0 ? 0 : -1;
}

/*
 * Launch after the HTTP response so the shell deeplink path does not wait on
 * hbldr (slow handoff often surfaces as a "not supported" flash).
 */
static void *pp_launch_player_thread(void *arg) {
  (void)arg;
  if (pp_launch_player() != 0) {
    pp_log("async launch failed");
    pp_notify("PS Play launch failed");
  }
  return NULL;
}

static int pp_launch_player_async(void) {
  pthread_t th;
  pthread_attr_t attr;
  int rc;

  if (pthread_attr_init(&attr) != 0)
    return pp_launch_player();
  (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  rc = pthread_create(&th, &attr, pp_launch_player_thread, NULL);
  (void)pthread_attr_destroy(&attr);
  if (rc != 0) {
    pp_log("async launch thread failed rc=%d — sync fallback", rc);
    return pp_launch_player();
  }
  return 0;
}

static void pp_try_unlink(const char *path) {
  if (path && unlink(path) == 0)
    pp_log("deleted %s", path);
}

static void pp_try_rmdir(const char *path) {
  if (path && rmdir(path) == 0)
    pp_log("rmdir %s", path);
}

static void pp_wipe_user_tile(const char *title_id) {
  char path[320];

  if (!title_id || !title_id[0])
    return;

  snprintf(path, sizeof(path), "%s/%s/sce_sys/param.json", PP_APP_ROOT, title_id);
  pp_try_unlink(path);
  snprintf(path, sizeof(path), "%s/%s/sce_sys/icon0.png", PP_APP_ROOT, title_id);
  pp_try_unlink(path);
  snprintf(path, sizeof(path), "%s/%s/sce_sys", PP_APP_ROOT, title_id);
  pp_try_rmdir(path);
  snprintf(path, sizeof(path), "%s/%s/eboot.bin", PP_APP_ROOT, title_id);
  pp_try_unlink(path);
  snprintf(path, sizeof(path), "%s/%s/eboot.elf", PP_APP_ROOT, title_id);
  pp_try_unlink(path);
  snprintf(path, sizeof(path), "%s/%s", PP_APP_ROOT, title_id);
  pp_try_rmdir(path);
}

static void pp_wipe_runtime(void) {
  pp_try_unlink(PP_PLAYER_PATH);
  pp_try_unlink(PP_RUNTIME_DIR "/sce_sys/param.json");
  pp_try_unlink(PP_RUNTIME_DIR "/sce_sys/icon0.png");
  pp_try_rmdir(PP_RUNTIME_DIR "/sce_sys");
  pp_try_rmdir(PP_RUNTIME_DIR);
}

static void pp_stop_running_bigapp(void) {
  int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
  int wait_attempt;

  if (app_id <= 0)
    return;

  pp_log("stopping running BigApp id=0x%x", app_id);
  if (sceSystemServiceKillApp(app_id, -1, 0, 0) != 0) {
    pp_log("KillApp failed");
    return;
  }
  for (wait_attempt = 0; wait_attempt < 15; wait_attempt++) {
    if (sceKernelGetAppState(app_id, 0, 0) != 0)
      break;
    usleep(200000);
  }
}

static int pp_appinst_uninstall_titles(void) {
  int i, rc, last = -1;

  if (sceAppInstUtilInitialize() != 0) {
    pp_log("AppInstUtilInitialize failed (uninstall)");
    return -1;
  }

  for (i = 0; PP_LEGACY_TITLES[i]; i++) {
    rc = sceAppInstUtilAppUnInstall(PP_LEGACY_TITLES[i], NULL, NULL);
    pp_log("UnInstall %s -> 0x%08x", PP_LEGACY_TITLES[i], rc);
    usleep(150000);
  }

  rc = sceAppInstUtilAppUnInstall(PP_TITLE_ID, NULL, NULL);
  pp_log("UnInstall %s -> 0x%08x", PP_TITLE_ID, rc);
  last = rc;
  usleep(250000);

  (void)sceAppInstUtilTerminate();
  /* 0 = success; non-zero may still mean already gone — files wiped next */
  return last;
}

/*
 * Full removal of Prospero Media tile from home.
 * Sony often hides Options→Delete for Media BigApp hosts; this is the
 * supported uninstall path (launcher route + one-shot ELF + Settings).
 */
static int pp_uninstall_media_tile(void) {
  int appinst_rc;

  pp_log("uninstall begin title=%s", PP_TITLE_ID);
  pp_stop_running_bigapp();

  appinst_rc = pp_with_appinst_authid(pp_appinst_uninstall_titles);
  pp_log("appinst uninstall wrapper rc=%d", appinst_rc);

  pp_wipe_user_tile(PP_TITLE_ID);
  for (int i = 0; PP_LEGACY_TITLES[i]; i++)
    pp_wipe_user_tile(PP_LEGACY_TITLES[i]);

  if (hbldr_remove_host() != 0)
    pp_log("host remove incomplete (may need remount/retry)");
  else
    pp_log("system host removed /system_ex/app/%s", PP_TITLE_ID);

  pp_wipe_runtime();
  pp_log("uninstall complete");
  pp_notify("PS Play removed from Media");
  return 0;
}

static void pp_serve(int listen_fd) {
  for (;;) {
    char req[2048];
    int cfd = accept(listen_fd, NULL, NULL);
    ssize_t n;

    if (cfd < 0)
      continue;
    n = recv(cfd, req, sizeof(req) - 1, 0);
    if (n > 0)
      req[n] = 0;
    else
      req[0] = 0;

    if (strncmp(req, "GET /launch", 11) == 0) {
      pp_log("route=/launch");
      /*
       * Answer 204 first: the home shell treats a slow/failed deeplink as
       * "not supported". Player start continues on a worker thread.
       */
      if (pp_launch_player_async() == 0)
        pp_http_reply_html(cfd, 200, pp_launch_html);
      else
        pp_http_reply(cfd, 500, "launch failed\n");
    } else if (strncmp(req, "GET /uninstall", 14) == 0) {
      pp_log("route=/uninstall");
      (void)pp_uninstall_media_tile();
      /* 204 so a tile/browser hit does not leave an "ok" page */
      pp_http_reply(cfd, 204, "");
      close(cfd);
      return;
    } else if (strncmp(req, "GET /status", 11) == 0) {
      pp_http_reply(cfd, 200, "prospero-media-ready\n");
    } else if (strncmp(req, "GET /shutdown", 13) == 0) {
      pp_http_reply(cfd, 200, "bye\n");
      close(cfd);
      return;
    } else {
      pp_http_reply(cfd, 404, "");
    }
    close(cfd);
  }
}

int main(void) {
  int listen_fd;

  (void)signal(SIGPIPE, SIG_IGN);
  pp_log("ProsperoPlayer %s media launcher start id=%s port=%d", PP_VERSION,
         PP_TITLE_ID, PP_SERVICE_PORT);
  pp_notify("PS Play " PP_VERSION " launcher\nstarting...");

  if (pp_player_elf_size < 64 || pp_player_elf[0] != 0x7f) {
    pp_log("embedded player invalid");
    pp_notify("PS Play FAILED step 2\nembedded player invalid");
    return 2;
  }
  if (pp_install_runtime() != 0) {
    pp_notify("PS Play FAILED step 3\nruntime install (errno in log)");
    return 3;
  }
  pp_notify("PS Play step 3 ok\nruntime installed");

  (void)sceUserServiceInitialize(NULL);

  /* System BigApp host must exist before the tile is published */
  if (hbldr_prepare_host() != 0) {
    char msg[128];
    pp_log("host prepare failed errno=%d", errno);
    snprintf(msg, sizeof(msg),
             "ProsperoPlayer FAILED step 4\nhost prepare errno=%d", errno);
    pp_notify(msg);
    (void)sceUserServiceTerminate();
    return 4;
  }
  pp_log("system host ready /system_ex/app/%s", PP_TITLE_ID);
  pp_notify("PS Play step 4 ok\nsystem host ready");

  if (pp_with_appinst_authid(pp_register_media_tile) != 0) {
    pp_log("media tile registration failed");
    pp_notify("PS Play FAILED step 6\ntile registration (see log)");
    (void)sceUserServiceTerminate();
    return 6;
  }
  pp_notify("PS Play step 6 ok\nMedia tile registered");

  listen_fd = pp_bind_loopback();
  if (listen_fd < 0) {
    pp_log("bind 127.0.0.1:%d failed errno=%d", PP_SERVICE_PORT, errno);
    pp_notify("PS Play launcher failed\nPort busy");
    (void)sceUserServiceTerminate();
    return 5;
  }

  pp_log("ready — open PS Play from Media (keep this payload resident)");
  /* Toast so user knows the tile is live (especially after autoload). */
  pp_notify_ready();
  pp_serve(listen_fd);
  close(listen_fd);
  (void)sceUserServiceTerminate();
  payload_exit(0);
  return 0;
}
