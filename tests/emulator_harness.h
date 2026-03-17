#ifndef EMULATOR_HARNESS_H
#define EMULATOR_HARNESS_H

#include <stddef.h>
#include <sys/types.h>

#define ZX3_EMU_DEFAULT_HOST "127.0.0.1"
#define ZX3_EMU_DEFAULT_PORT 10000

int zx3_resolve_emulator(char* out, size_t out_sz);
int zx3_wait_for_port(const char* host, int port, int timeout_ms);
int zx3_zrcp_command(const char* host, int port, const char* cmd,
                     char* out, size_t out_sz);
int zx3_spawn_emulator(const char* emulator_path, int port, pid_t* out_pid);
void zx3_stop_emulator(pid_t pid, const char* host, int port);

#endif
