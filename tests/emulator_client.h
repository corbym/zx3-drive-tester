#ifndef EMULATOR_CLIENT_H
#define EMULATOR_CLIENT_H

#include <stddef.h>

typedef struct emulator_client emulator_client_t;

/* Lifecycle: start emulator, get client handle. Stop when done. */
emulator_client_t* emulator_client_start(void);
void emulator_client_stop(emulator_client_t* client);

/* High-level emulator operations. */
int emulator_close_all_menus(emulator_client_t* client);
int emulator_hard_reset(emulator_client_t* client);
int emulator_smartload(emulator_client_t* client, const char* path);
int emulator_send_key(emulator_client_t* client, unsigned char key);
int emulator_get_ocr(emulator_client_t* client, char* ocr_out, size_t ocr_sz);
int emulator_wait_for_ocr(emulator_client_t* client, const char* marker1,
                          const char* marker2, int timeout_ms,
                          char* ocr_out, size_t ocr_sz);
/* Save a screenshot to an absolute path (BMP format). Polls up to 3s for the
 * file to appear. The directory must already exist. */
int emulator_save_screen(emulator_client_t* client, const char* path);

#endif
