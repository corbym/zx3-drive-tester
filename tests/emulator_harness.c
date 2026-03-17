#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "emulator_harness.h"

#define DEFAULT_MACHINE "P340"
#define DEFAULT_EMU_PATH "/Applications/zesarux.app/Contents/MacOS/zesarux"

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

static int find_in_path(const char* prog, char* out, size_t out_sz) {
  const char* path = getenv("PATH");
  const char* p;
  const char* seg;
  size_t seg_len;

  if (!path || !*path) return 0;
  p = path;
  while (*p) {
    seg = p;
    while (*p && *p != ':') p++;
    seg_len = (size_t)(p - seg);
    if (seg_len > 0) {
      char candidate[1024];
      if (seg_len + 1 + strlen(prog) + 1 < sizeof(candidate)) {
        memcpy(candidate, seg, seg_len);
        candidate[seg_len] = '/';
        strcpy(candidate + seg_len + 1, prog);
        if (access(candidate, X_OK) == 0) {
          strncpy(out, candidate, out_sz - 1U);
          out[out_sz - 1U] = '\0';
          return 1;
        }
      }
    }
    if (*p == ':') p++;
  }
  return 0;
}

int zx3_resolve_emulator(char* out, size_t out_sz) {
  const char* env = getenv("ZESARUX_BIN");
  if (env && *env && access(env, X_OK) == 0) {
    strncpy(out, env, out_sz - 1U);
    out[out_sz - 1U] = '\0';
    return 1;
  }
  if (access(DEFAULT_EMU_PATH, X_OK) == 0) {
    strncpy(out, DEFAULT_EMU_PATH, out_sz - 1U);
    out[out_sz - 1U] = '\0';
    return 1;
  }
  return find_in_path("zesarux", out, out_sz);
}

int zx3_wait_for_port(const char* host, int port, int timeout_ms) {
  long long deadline = now_ms() + timeout_ms;
  while (now_ms() < deadline) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    struct timeval tv;
    if (fd < 0) return 0;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
      close(fd);
      return 0;
    }
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
      close(fd);
      return 1;
    }
    close(fd);
    usleep(100000);
  }
  return 0;
}

int zx3_zrcp_command(const char* host, int port, const char* cmd,
                     char* out, size_t out_sz) {
  int fd;
  struct sockaddr_in addr;
  struct timeval tv;
  char sendbuf[512];
  size_t sendlen;
  size_t used = 0;

  out[0] = '\0';
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
    close(fd);
    return -1;
  }

  tv.tv_sec = 2;
  tv.tv_usec = 0;
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }

  snprintf(sendbuf, sizeof(sendbuf), "%s\n", cmd);
  sendlen = strlen(sendbuf);
  if (send(fd, sendbuf, sendlen, 0) < 0) {
    close(fd);
    return -1;
  }

  while (used + 1U < out_sz) {
    ssize_t n = recv(fd, out + used, out_sz - used - 1U, 0);
    if (n <= 0) break;
    used += (size_t)n;
  }
  out[used] = '\0';
  close(fd);
  return 0;
}

int zx3_spawn_emulator(const char* emulator_path, int port, pid_t* out_pid) {
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    char port_s[16];
    char* argv[20];
    int i = 0;

    setenv("HOME", "/tmp/zesarux-smoketest-home-c", 1);
    snprintf(port_s, sizeof(port_s), "%d", port);

    argv[i++] = (char*)emulator_path;
    argv[i++] = "--machine";
    argv[i++] = DEFAULT_MACHINE;
    argv[i++] = "--emulatorspeed";
    argv[i++] = "100";
    argv[i++] = "--fastautoload";
    argv[i++] = "--enable-remoteprotocol";
    argv[i++] = "--remoteprotocol-port";
    argv[i++] = port_s;
    argv[i++] = "--noconfigfile";
    argv[i++] = "--vo";
    argv[i++] = "null";
    argv[i++] = "--ao";
    argv[i++] = "null";
    argv[i] = NULL;

    execv(emulator_path, argv);
    _exit(127);
  }
  *out_pid = pid;
  return 0;
}

void zx3_stop_emulator(pid_t pid, const char* host, int port) {
  char out[1024];
  int status;
  int i;

  (void)zx3_zrcp_command(host, port, "exit-emulator", out, sizeof(out));
  for (i = 0; i < 50; i++) {
    if (waitpid(pid, &status, WNOHANG) == pid) return;
    usleep(100000);
  }
  kill(pid, SIGTERM);
  usleep(300000);
  if (waitpid(pid, &status, WNOHANG) == 0) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
  }
}


