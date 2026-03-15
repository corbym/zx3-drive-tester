#include "menu_system.h"

#include <ctype.h>

extern unsigned char inportb(unsigned short port);

static const MenuItem MENU_ITEMS[] = {
    {'M', "M Motor and drive status", 1},
    {'P', "P Drive read ID probe", 1},
    {'K', "K Recalibrate and seek track 2", 1},
    {'I', "I Interactive step seek", 1},
    {'T', "T Read ID on track 0", 1},
    {'D', "D Read track data loop", 1},
    {'H', "H Disk RPM check loop", 1},
    {'A', "A Run all core tests", 1},
    {'R', "R Show report card", 1},
    {'C', "C Clear stored results", 1},
    {'Q', "Q Quit", 1},
};

typedef struct {
  unsigned short row_port;
  unsigned char bit_mask;
  char key;
} KeyMap;

static const KeyMap menu_keymap[] = {
    {0xFEFE, 0x10, 'V'},
    {0xFDFE, 0x08, 'F'},
    {0xFBFE, 0x02, 'W'},
    {0xFDFE, 0x02, 'S'},
    {0xBFFE, 0x01, '\n'},
    {0x7FFE, 0x01, ' '},
    {0xFEFE, 0x04, 'X'},
    {0xFBFE, 0x01, 'Q'},
    {0xFDFE, 0x01, 'A'},
    {0x7FFE, 0x08, 'C'},
    {0xFBFE, 0x08, 'R'},
    {0xBFFE, 0x04, 'K'},
    {0xFDFE, 0x04, 'D'},
    {0xBFFE, 0x08, 'J'},
    {0xFBFE, 0x10, 'T'},
    {0x7FFE, 0x04, 'M'},
    {0xDFFE, 0x01, 'P'},
    {0xDFFE, 0x04, 'I'},
    {0xBFFE, 0x10, 'H'},
    {0xEFFE, 0x10, '6'},
    {0xEFFE, 0x08, '7'},
};

static unsigned char break_pressed(void) {
  unsigned char caps_shift = (unsigned char)(inportb(0xFEFE) & 0x01);
  unsigned char space = (unsigned char)(inportb(0x7FFE) & 0x01);
  return (unsigned char)((caps_shift == 0) && (space == 0));
}

static unsigned char caps_shift_pressed(void) {
  return (unsigned char)((inportb(0xFEFE) & 0x01) == 0);
}

static unsigned char key6_pressed(void) {
  return (unsigned char)((inportb(0xEFFE) & 0x10) == 0);
}

static unsigned char key7_pressed(void) {
  return (unsigned char)((inportb(0xEFFE) & 0x08) == 0);
}

static unsigned char w_pressed(void) {
  return (unsigned char)((inportb(0xFBFE) & 0x02) == 0);
}

static unsigned char s_pressed(void) {
  return (unsigned char)((inportb(0xFDFE) & 0x02) == 0);
}

static unsigned char f_pressed(void) {
  return (unsigned char)((inportb(0xFDFE) & 0x08) == 0);
}

static unsigned char v_pressed(void) {
  return (unsigned char)((inportb(0xFEFE) & 0x10) == 0);
}

static unsigned char menu_up_pressed(void) {
  return (unsigned char)((caps_shift_pressed() && key7_pressed()) ||
                         w_pressed() || f_pressed());
}

static unsigned char menu_down_pressed(void) {
  return (unsigned char)((caps_shift_pressed() && key6_pressed()) ||
                         s_pressed() || v_pressed());
}

static void wait_for_key_release(unsigned short row_port, unsigned char bit_mask) {
  while ((inportb(row_port) & bit_mask) == 0) {
  }
}

static void wait_for_menu_up_release(void) {
  while (menu_up_pressed()) {
  }
}

static void wait_for_menu_down_release(void) {
  while (menu_down_pressed()) {
  }
}

const MenuItem *menu_items(void) { return MENU_ITEMS; }

unsigned char menu_item_count(void) {
  return (unsigned char)(sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]));
}

char menu_key_for_index(unsigned char index) {
  if (index >= menu_item_count()) return 'Q';
  return MENU_ITEMS[index].key;
}

unsigned char menu_index_for_key(char key, unsigned char *found) {
  unsigned char i;
  char up = (char)toupper((unsigned char)key);

  for (i = 0; i < menu_item_count(); i++) {
    if (MENU_ITEMS[i].key == up) {
      *found = 1;
      return i;
    }
  }

  *found = 0;
  return 0;
}

int read_menu_key_blocking(void) {
  unsigned int i;

  for (;;) {
    if (break_pressed()) {
      while (break_pressed()) {
      }
      return 27;
    }

    if (menu_up_pressed()) {
      wait_for_menu_up_release();
      return MENU_KEY_UP;
    }

    if (menu_down_pressed()) {
      wait_for_menu_down_release();
      return MENU_KEY_DOWN;
    }

    for (i = 0; i < sizeof(menu_keymap) / sizeof(menu_keymap[0]); i++) {
      if ((inportb(menu_keymap[i].row_port) & menu_keymap[i].bit_mask) == 0) {
        char key = menu_keymap[i].key;
        wait_for_key_release(menu_keymap[i].row_port, menu_keymap[i].bit_mask);
        if (key == 'F' || key == 'W') return MENU_KEY_UP;
        if (key == 'V' || key == 'S') return MENU_KEY_DOWN;
        return key;
      }
    }
  }
}
