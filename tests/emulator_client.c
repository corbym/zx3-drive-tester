#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>

#include "emulator_client.h"
#include "emulator_harness.h"

struct emulator_client {
  pid_t emu_pid;
  const char* host;
  int port;
};

emulator_client_t* emulator_client_start(void) {
  emulator_client_t* client;
  char emulator[1024];

  client = (emulator_client_t*)malloc(sizeof(*client));
  if (!client) return NULL;

  if (!zx3_resolve_emulator(emulator, sizeof(emulator))) {
    fprintf(stderr, "    emulator not found (set ZESARUX_BIN or install zesarux)\n");
    free(client);
    return NULL;
  }

  if (zx3_spawn_emulator(emulator, ZX3_EMU_DEFAULT_PORT, &client->emu_pid) != 0) {
    fprintf(stderr, "    failed to spawn emulator: %s\n", emulator);
    free(client);
    return NULL;
  }

  if (!zx3_wait_for_port(ZX3_EMU_DEFAULT_HOST, ZX3_EMU_DEFAULT_PORT, 15000)) {
    fprintf(stderr, "    emulator did not open ZRCP port\n");
    zx3_stop_emulator(client->emu_pid, ZX3_EMU_DEFAULT_HOST, ZX3_EMU_DEFAULT_PORT);
    free(client);
    return NULL;
  }

  client->host = ZX3_EMU_DEFAULT_HOST;
  client->port = ZX3_EMU_DEFAULT_PORT;

  return client;
}

#define ZRCP_BUF_SIZE 65536

void emulator_client_stop(emulator_client_t* client) {
  if (!client) return;
  zx3_stop_emulator(client->emu_pid, client->host, client->port);
  free(client);
}

int emulator_close_all_menus(emulator_client_t* client) {
  char ocr[ZRCP_BUF_SIZE];
  if (!client) return -1;
  return zx3_zrcp_command(client->host, client->port, "close-all-menus", ocr,
                          sizeof(ocr));
}

int emulator_hard_reset(emulator_client_t* client) {
  char ocr[ZRCP_BUF_SIZE];
  if (!client) return -1;
  return zx3_zrcp_command(client->host, client->port, "hard-reset-cpu", ocr,
                          sizeof(ocr));
}

int emulator_smartload(emulator_client_t* client, const char* path) {
  char cmd[1024];
  char ocr[ZRCP_BUF_SIZE];
  if (!client || !path) return -1;
  snprintf(cmd, sizeof(cmd), "smartload %s", path);
  return zx3_zrcp_command(client->host, client->port, cmd, ocr, sizeof(ocr));
}

int emulator_send_key(emulator_client_t* client, unsigned char key) {
  char cmd[64];
  char ocr[ZRCP_BUF_SIZE];
  if (!client) return -1;
  snprintf(cmd, sizeof(cmd), "send-keys-ascii 25 %u", (unsigned int)key);
  return zx3_zrcp_command(client->host, client->port, cmd, ocr, sizeof(ocr));
}

int emulator_get_ocr(emulator_client_t* client, char* ocr_out, size_t ocr_sz) {
  if (!client || !ocr_out || ocr_sz == 0) return -1;
  return zx3_zrcp_command(client->host, client->port, "get-ocr", ocr_out,
                          ocr_sz);
}

int emulator_save_screen(emulator_client_t* client, const char* path) {
  char cmd[2048];
  char rsp[ZRCP_BUF_SIZE];
  int i;
  if (!client || !path) return -1;
  if (snprintf(cmd, sizeof(cmd), "save-screen %s", path) >= (int)sizeof(cmd))
    return -1;
  if (zx3_zrcp_command(client->host, client->port, cmd, rsp, sizeof(rsp)) != 0)
    return -1;
  /* Poll until the file appears on disk (up to 3 seconds). */
  for (i = 0; i < 30; i++) {
    usleep(100000);
    if (access(path, F_OK) == 0) return 0;
  }
  return -1;
}

int emulator_wait_for_ocr(emulator_client_t* client, const char* marker1,
                          const char* marker2, int timeout_ms,
                          char* ocr_out, size_t ocr_sz) {
  long long deadline;
  long long now_val;
  struct timespec ts;

  if (!client || !marker1 || !marker2 || !ocr_out || ocr_sz == 0) return -1;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  deadline = (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL +
             timeout_ms;

  while (1) {
    clock_gettime(CLOCK_MONOTONIC, &ts);
    now_val =
        (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;

    if (now_val >= deadline) return -1;

    if (emulator_get_ocr(client, ocr_out, ocr_sz) == 0 &&
        strstr(ocr_out, marker1) != NULL &&
        strstr(ocr_out, marker2) != NULL) {
      return 0;
    }

    usleep(100000);
  }
}
