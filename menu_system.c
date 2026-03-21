#include "menu_system.h"
#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

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
    const unsigned char caps_shift = (unsigned char) (inportb(0xFEFE) & 0x01);
    unsigned char space = (unsigned char) (inportb(0x7FFE) & 0x01);
    return (unsigned char) ((caps_shift == 0) && (space == 0));
}

static unsigned char caps_shift_pressed(void) {
    return (unsigned char) ((inportb(0xFEFE) & 0x01) == 0);
}

static unsigned char key6_pressed(void) {
    return (unsigned char) ((inportb(0xEFFE) & 0x10) == 0);
}

static unsigned char key7_pressed(void) {
    return (unsigned char) ((inportb(0xEFFE) & 0x08) == 0);
}

static unsigned char w_pressed(void) {
    return (unsigned char) ((inportb(0xFBFE) & 0x02) == 0);
}

static unsigned char s_pressed(void) {
    return (unsigned char) ((inportb(0xFDFE) & 0x02) == 0);
}

static unsigned char f_pressed(void) {
    return (unsigned char) ((inportb(0xFDFE) & 0x08) == 0);
}

static unsigned char v_pressed(void) {
    return (unsigned char) ((inportb(0xFEFE) & 0x10) == 0);
}

static unsigned char menu_up_pressed(void) {
    return (unsigned char) ((caps_shift_pressed() && key7_pressed()) ||
                            w_pressed() || f_pressed());
}

static unsigned char menu_down_pressed(void) {
    return (unsigned char) ((caps_shift_pressed() && key6_pressed()) ||
                            s_pressed() || v_pressed());
}

const MenuItem *menu_items(void) { return MENU_ITEMS; }

unsigned char menu_item_count(void) {
    return (unsigned char) (sizeof(MENU_ITEMS) / sizeof(MENU_ITEMS[0]));
}

char menu_key_for_index(unsigned char index) {
    if (index >= menu_item_count()) return 'Q';
    return MENU_ITEMS[index].key;
}

unsigned char menu_index_for_key(char key, unsigned char *found) {
    const char up = (char) toupper((unsigned char)key);

    for (unsigned char i = 0; i < menu_item_count(); i++) {
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
            *selected_index = (unsigned char) (*selected_index - 1U);
            *selection_changed = 1;
        }
        return 0;
    }

    if (key == MENU_KEY_DOWN) {
        if ((unsigned char) (*selected_index + 1U) < menu_item_count()) {
            *selected_index = (unsigned char) (*selected_index + 1U);
            *selection_changed = 1;
        }
        return 0;
    }

    if (key == '\n') {
        key = menu_key_for_index(*selected_index);
    }

    key = toupper((unsigned char)key);
    index = menu_index_for_key((char) key, &found);
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
    for (;;) {
        if (break_pressed()) {
            if (!menu_break_latched) {
                menu_break_latched = 1;
                return 27;
            }
        }
        else {
            menu_break_latched = 0;
        }

        if (menu_up_pressed()) {
            if (!menu_up_latched) {
                menu_up_latched = 1;
                return MENU_KEY_UP;
            }
        }
        else {
            menu_up_latched = 0;
        }

        if (menu_down_pressed()) {
            if (!menu_down_latched) {
                menu_down_latched = 1;
                return MENU_KEY_DOWN;
            }
        }
        else {
            menu_down_latched = 0;
        }

        for (unsigned int i = 0; i < MENU_KEYMAP_COUNT; i++) {
            unsigned char pressed = (unsigned char) ((inportb(menu_keymap[i].row_port) &
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

/* -------------------------------------------------------------------------- */
/* Menu rendering                                                              */
/* -------------------------------------------------------------------------- */

static void menu_apply_row_visual(unsigned char index, unsigned char selected) {
    const MenuItem *items = menu_items();
    unsigned char count = menu_item_count();
    unsigned char row;
    unsigned char col;
    unsigned char paper;

    if (index >= count) return;

    row = (unsigned char)(3U + index);
    paper = selected ? ZX_COLOUR_CYAN : ZX_COLOUR_WHITE;
    for (col = 0; col < 32U; col++) {
        ui_attr_set_cell(row, col, ZX_COLOUR_BLACK, paper, 1);
    }
    if (items[index].hot_col < 31U) {
        ui_attr_set_cell(row, (unsigned char)(items[index].hot_col + 1U),
                         ZX_COLOUR_BLUE, paper, 1);
    }
    ui_screen_put_char(row, 31, selected ? '~' : ' ');
}

void menu_update_selection(unsigned char old_index, unsigned char new_index) {
    if (old_index == new_index) return;
    menu_apply_row_visual(old_index, 0);
    menu_apply_row_visual(new_index, 1);
}

void menu_render_full(unsigned char selected_index, unsigned char total_pass) {
    const MenuItem *items = menu_items();
    unsigned char count = menu_item_count();
    unsigned char i;
    unsigned char col;
    char status_line[29];
    static const unsigned char STRIPE_PAPER[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    static const unsigned char STRIPE_INK[8]   = {7, 7, 7, 7, 0, 0, 0, 0};

    ui_reset_text_screen_cache();

    ui_term_clear();
    printf(" ZX +3 DISK TESTER\n");
    if (total_pass == 0) {
        strcpy(status_line, "STATUS: NO TESTS RUN");
    } else {
        sprintf(status_line, "STATUS: %u/5 PASS", (unsigned int)total_pass);
    }
    printf("%s\n\n", status_line);

    for (i = 0; i < count; i++) {
        printf(" %s\n", items[i].label);
    }

    printf("\nUP   : W/F/CAPS+7\n");
    printf("DOWN : S/V/CAPS+6\n");
    printf("ENTER: SELECT  Q: QUIT\n");

    /* Reapply +3-style colour layout on top of terminal text output. */
    ui_attr_fill(ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 0);
    for (col = 0; col < 32; col++) {
        ui_attr_set_cell(0, col,  ZX_COLOUR_WHITE, ZX_COLOUR_BLACK, 1);
        ui_attr_set_cell(15, col, ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 1);
        ui_attr_set_cell(16, col, ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 1);
    }
    for (col = 0; col < 8; col++) {
        ui_attr_set_cell(0, (unsigned char)(24 + col),
                         STRIPE_INK[col], STRIPE_PAPER[col], 1);
    }
    for (i = 0; i < count; i++) {
        menu_apply_row_visual(i, (unsigned char)(i == selected_index));
    }
}

