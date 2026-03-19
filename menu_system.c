#include "menu_system.h"

#include <ctype.h>

#if COMPACT_UI
typedef struct {
  unsigned char ch;
  unsigned char rows[8];
} CompactGlyph;

static void set_compact_glyph(unsigned char* font, unsigned char ch,
                              const unsigned char rows[8]) {
  unsigned char* glyph = &font[((unsigned short)ch) * 8U];
  unsigned char r;

  for (r = 0; r < 8; r++) {
    glyph[r] = rows[r];
  }
}

static void copy_font_glyph(unsigned char* font, unsigned char dst,
                            unsigned char src) {
  memcpy(&font[((unsigned short)dst) * 8U], &font[((unsigned short)src) * 8U],
         8U);
}

static void apply_compact_font(unsigned char* font) {
  unsigned char ch;
  unsigned int i;
  /*
   * Source: epto/epto-fonts (digital-6x6), GPL-2.0 repository.
   * We use an ASCII subset required by the UI.
   */
  static const unsigned char fallback_rows[8] = {0x00, 0x7E, 0xA5, 0x99,
                                                 0x99, 0xA5, 0x7E, 0x00};
  static const CompactGlyph glyphs[] = {
      {' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
      {'!', {0x00, 0x18, 0x3C, 0x18, 0x00, 0x18, 0x00, 0x00}},
      {'"', {0x00, 0x28, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00}},
      {'#', {0x00, 0x10, 0x3C, 0x7C, 0x3C, 0x10, 0x00, 0x00}},
      {'+', {0x00, 0x10, 0x10, 0x7C, 0x10, 0x10, 0x00, 0x00}},
      {',', {0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x30, 0x00}},
      {'-', {0x00, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x00, 0x00}},
      {'.', {0x00, 0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00}},
      {'/', {0x00, 0x06, 0x0C, 0x18, 0x30, 0x60, 0x00, 0x00}},
      {'0', {0x00, 0x7C, 0x44, 0x64, 0x64, 0x7C, 0x00, 0x00}},
      {'1', {0x00, 0x10, 0x10, 0x18, 0x18, 0x18, 0x00, 0x00}},
      {'2', {0x00, 0x7C, 0x04, 0x7C, 0x60, 0x7C, 0x00, 0x00}},
      {'3', {0x00, 0x78, 0x08, 0x7C, 0x0C, 0x7C, 0x00, 0x00}},
      {'4', {0x00, 0x50, 0x50, 0x7C, 0x18, 0x18, 0x00, 0x00}},
      {'5', {0x00, 0x7C, 0x40, 0x7C, 0x0C, 0x7C, 0x00, 0x00}},
      {'6', {0x00, 0x7C, 0x40, 0x7C, 0x4C, 0x7C, 0x00, 0x00}},
      {'7', {0x00, 0x78, 0x08, 0x0C, 0x0C, 0x0C, 0x00, 0x00}},
      {'8', {0x00, 0x78, 0x48, 0x7C, 0x4C, 0x7C, 0x00, 0x00}},
      {'9', {0x00, 0x78, 0x48, 0x7C, 0x0C, 0x7C, 0x00, 0x00}},
      {':', {0x00, 0x18, 0x18, 0x00, 0x18, 0x18, 0x00, 0x00}},
      {';', {0x00, 0x00, 0x0C, 0x00, 0x0C, 0x0C, 0x18, 0x00}},
      {'=', {0x00, 0x00, 0x7C, 0x00, 0x7C, 0x00, 0x00, 0x00}},
      {'?', {0x00, 0x18, 0x24, 0x0C, 0x18, 0x00, 0x18, 0x00}},
      {'A', {0x00, 0x7C, 0x44, 0x7C, 0x64, 0x64, 0x00, 0x00}},
      {'B', {0x00, 0x78, 0x44, 0x78, 0x64, 0x78, 0x00, 0x00}},
      {'C', {0x00, 0x7C, 0x40, 0x60, 0x60, 0x7C, 0x00, 0x00}},
      {'D', {0x00, 0x78, 0x44, 0x64, 0x64, 0x78, 0x00, 0x00}},
      {'E', {0x00, 0x7C, 0x40, 0x78, 0x60, 0x7C, 0x00, 0x00}},
      {'F', {0x00, 0x7C, 0x40, 0x78, 0x60, 0x60, 0x00, 0x00}},
      {'G', {0x00, 0x7C, 0x40, 0x6C, 0x64, 0x7C, 0x00, 0x00}},
      {'H', {0x00, 0x44, 0x44, 0x7C, 0x64, 0x64, 0x00, 0x00}},
      {'I', {0x00, 0x7C, 0x10, 0x18, 0x18, 0x7C, 0x00, 0x00}},
      {'J', {0x00, 0x7C, 0x04, 0x0C, 0x4C, 0x7C, 0x00, 0x00}},
      {'K', {0x00, 0x44, 0x48, 0x70, 0x68, 0x64, 0x00, 0x00}},
      {'L', {0x00, 0x40, 0x40, 0x60, 0x60, 0x7C, 0x00, 0x00}},
      {'M', {0x00, 0x7C, 0x74, 0x74, 0x54, 0x44, 0x00, 0x00}},
      {'N', {0x00, 0x7C, 0x44, 0x64, 0x64, 0x64, 0x00, 0x00}},
      {'O', {0x00, 0x7C, 0x44, 0x64, 0x64, 0x7C, 0x00, 0x00}},
      {'P', {0x00, 0x7C, 0x44, 0x7C, 0x60, 0x60, 0x00, 0x00}},
      {'Q', {0x00, 0x7C, 0x44, 0x64, 0x6C, 0x7E, 0x00, 0x00}},
      {'R', {0x00, 0x7C, 0x44, 0x7C, 0x68, 0x64, 0x00, 0x00}},
      {'S', {0x00, 0x7C, 0x40, 0x7C, 0x0C, 0x7C, 0x00, 0x00}},
      {'T', {0x00, 0x7C, 0x10, 0x18, 0x18, 0x18, 0x00, 0x00}},
      {'U', {0x00, 0x44, 0x44, 0x64, 0x64, 0x7C, 0x00, 0x00}},
      {'V', {0x00, 0x44, 0x6C, 0x28, 0x38, 0x10, 0x00, 0x00}},
      {'W', {0x00, 0x44, 0x54, 0x74, 0x74, 0x7C, 0x00, 0x00}},
      {'X', {0x00, 0x6C, 0x38, 0x10, 0x38, 0x6C, 0x00, 0x00}},
      {'Y', {0x00, 0x6C, 0x38, 0x10, 0x18, 0x18, 0x00, 0x00}},
      {'Z', {0x00, 0x7C, 0x0C, 0x18, 0x30, 0x7C, 0x00, 0x00}},
      {'[', {0x00, 0x3C, 0x30, 0x30, 0x30, 0x3C, 0x00, 0x00}},
      {'\\', {0x00, 0x30, 0x18, 0x0C, 0x06, 0x02, 0x00, 0x00}},
      {']', {0x00, 0x3C, 0x0C, 0x0C, 0x0C, 0x3C, 0x00, 0x00}},
      {'_', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x7C, 0x00}},
      {'|', {0x00, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x7E, 0x00}},
  };

  /* Start from a fully custom font: no ROM glyph dependency in compact mode. */
  memset(font, 0x00, sizeof(font_ram));

  /* Seed printable range with a custom fallback to avoid random artifacts. */
  for (ch = 32; ch <= 126; ch++) {
    set_compact_glyph(font, ch, fallback_rows);
  }

  for (i = 0; i < sizeof(glyphs) / sizeof(glyphs[0]); i++) {
    set_compact_glyph(font, glyphs[i].ch, glyphs[i].rows);
  }

  /* Lowercase maps to uppercase to keep wording readable with a compact set. */
  for (ch = 'a'; ch <= 'z'; ch++) {
    copy_font_glyph(font, ch, (unsigned char)(ch - 'a' + 'A'));
  }
}
#endif

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

  enum { MENU_KEYMAP_COUNT = sizeof(menu_keymap) / sizeof(menu_keymap[0]) };
  static unsigned char menu_key_latched[MENU_KEYMAP_COUNT];
  static unsigned char menu_break_latched;
  static unsigned char menu_up_latched;
  static unsigned char menu_down_latched;

unsigned char break_pressed(void) {
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

int menu_resolve_action_key(int key, unsigned char *selected_index,
                            unsigned char *selection_changed) {
  unsigned char found = 0;
  unsigned char index = 0;

  if (!selected_index || !selection_changed) return 0;
  *selection_changed = 0;

  if (key == MENU_KEY_UP) {
    if (*selected_index > 0U) {
      *selected_index = (unsigned char)(*selected_index - 1U);
      *selection_changed = 1;
    }
    return 0;
  }

  if (key == MENU_KEY_DOWN) {
    if ((unsigned char)(*selected_index + 1U) < menu_item_count()) {
      *selected_index = (unsigned char)(*selected_index + 1U);
      *selection_changed = 1;
    }
    return 0;
  }

  if (key == '\n') {
    key = menu_key_for_index(*selected_index);
  }

  key = toupper((unsigned char)key);
  index = menu_index_for_key((char)key, &found);
  if (!found) {
    return 0;
  }

  if (index != *selected_index) {
    *selected_index = index;
    *selection_changed = 1;
  }

  return key;
}

int read_menu_key_blocking(void) {
  unsigned int i;
  unsigned char pressed;

  for (;;) {
    if (break_pressed()) {
      if (!menu_break_latched) {
        menu_break_latched = 1;
        return 27;
      }
    } else {
      menu_break_latched = 0;
    }

    if (menu_up_pressed()) {
      if (!menu_up_latched) {
        menu_up_latched = 1;
        return MENU_KEY_UP;
      }
    } else {
      menu_up_latched = 0;
    }

    if (menu_down_pressed()) {
      if (!menu_down_latched) {
        menu_down_latched = 1;
        return MENU_KEY_DOWN;
      }
    } else {
      menu_down_latched = 0;
    }

    for (i = 0; i < MENU_KEYMAP_COUNT; i++) {
      pressed = (unsigned char)((inportb(menu_keymap[i].row_port) &
                                 menu_keymap[i].bit_mask) == 0);
      if (pressed) {
        if (menu_key_latched[i]) {
          continue;
        }
        menu_key_latched[i] = 1;
        {
          char key = menu_keymap[i].key;
          if (key == 'F' || key == 'W') return MENU_KEY_UP;
          if (key == 'V' || key == 'S') return MENU_KEY_DOWN;
          return key;
        }
      }
      menu_key_latched[i] = 0;
    }
  }
}
