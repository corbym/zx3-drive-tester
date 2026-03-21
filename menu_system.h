#pragma once

typedef struct {
  char key;
  const char *label;
  unsigned char hot_col;
} MenuItem;

/* Shared keyboard-matrix descriptor used by both menu and runtime key scanners. */
typedef struct {
  unsigned short row_port;
  unsigned char bit_mask;
  char key;
} KeyMap;

#define MENU_KEY_UP (-2)
#define MENU_KEY_DOWN (-3)

const MenuItem *menu_items(void);
unsigned char menu_item_count(void);
char menu_key_for_index(unsigned char index);
unsigned char menu_index_for_key(char key, unsigned char *found);
int menu_resolve_action_key(int key, unsigned char *selected_index,
                            unsigned char *selection_changed);
int read_menu_key_blocking(void);
unsigned char break_pressed(void);
void menu_render_full(unsigned char selected_index, unsigned char total_pass);
void menu_update_selection(unsigned char old_index, unsigned char new_index);
