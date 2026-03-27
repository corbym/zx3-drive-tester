#include "menu_system.h"
#include "ui.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

extern unsigned char inportb(unsigned short port);
#if HEADLESS_ROM_FONT
static const MenuItem MENU_ITEMS[] = {
    {'M', "Motor ready test", 0},
    {'E', "Read ID Probe", 1},
    {'B', "Recalibrate test", 6},
    {'I', "Interactive seek", 0},
    {'T', "Read ID T0", 10},
    {'D', "Read data", 5},
    {'H', "Disk RPM check", 10},
    {'A', "Run all", 4},
    {'R', "Show report", 5},
    {'C', "Clear results", 0},
    {'Q', "Quit", 0},
};
#elif HEADLESS_ROM_FONT == 0
static const MenuItem MENU_ITEMS[] = {
    {'M', "MOTOR READY TEST", 0},
    {'E', "READ ID PROBE", 1},
    {'B', "RECALIBRATE TEST", 6},
    {'I', "INTERACTIVE SEEK", 0},
    {'T', "READ ID T0", 8},
    {'D', "READ DATA", 5},
    {'H', "DISK RPM CHECK", 10},
    {'A', "RUN ALL", 4},
    {'R', "SHOW REPORT", 5},
    {'C', "CLEAR RESULTS", 0},
    {'Q', "QUIT", 0},
};
#endif

static const KeyMap menu_keymap[] = {
    {0xBFFE, 0x01, '\n'},
    {0xFBFE, 0x01, 'Q'},
    {0xFDFE, 0x01, 'A'},
    {0x7FFE, 0x08, 'C'},
    {0xFBFE, 0x08, 'R'},
    {0xFDFE, 0x04, 'D'},
    {0xFBFE, 0x10, 'T'},
    {0x7FFE, 0x04, 'M'},
    {0xDFFE, 0x04, 'I'},
    {0xBFFE, 0x10, 'H'},
    {0xFBFE, 0x04, 'E'},
    {0x7FFE, 0x10, 'B'},
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

        for (unsigned char i = 0; i < MENU_KEYMAP_COUNT; i++) {
            unsigned char pressed = (unsigned char) ((inportb(menu_keymap[i].row_port) &
                                                      menu_keymap[i].bit_mask) == 0);
            if (pressed) {
                if (menu_key_latched[i]) {
                    continue;
                }
                menu_key_latched[i] = 1;
                {
                    char key = menu_keymap[i].key;
                    if (key == 'F') return MENU_KEY_UP;
                    if (key == 'V') return MENU_KEY_DOWN;
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

    if (index >= count) return;

    unsigned char row = (unsigned char) (1U + index);
    unsigned char paper = selected ? ZX_COLOUR_CYAN : ZX_COLOUR_WHITE;
    ui_attr_set_run(row, 0, 32, ZX_COLOUR_BLACK, paper, 1);
    if (items[index].hot_col < 31U) {
        ui_attr_set_cell(row, (unsigned char) (items[index].hot_col + 1U),
                         ZX_COLOUR_WHITE, ZX_COLOUR_BLACK, 1);
    }
    ui_screen_put_char(row, 31, selected ? '~' : ' ');
}

void menu_update_selection(unsigned char old_index, unsigned char new_index) {
    if (old_index == new_index) return;
    menu_apply_row_visual(old_index, 0);
    menu_apply_row_visual(new_index, 1);
}

static void menu_status_value_text(char *out, unsigned char total_pass) {
    if (!out) return;
    if (total_pass == 0U) {
        strcpy(out, "NOT RUN");
    }
    else {
        sprintf(out, "%u/5 PASS", (unsigned int) total_pass);
    }
}

void menu_render_full(unsigned char selected_index, unsigned char total_pass) {
    const MenuItem *items = menu_items();
    const unsigned char count = menu_item_count();
    unsigned char i;
    unsigned char col;
    char status_value[20];
    static const unsigned char STRIPE_PAPER[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    static const unsigned char STRIPE_INK[8] = {7, 7, 7, 7, 0, 0, 0, 0};

    ui_reset_text_screen_cache();

    menu_status_value_text(status_value, total_pass);

    ui_term_clear();
    printf(" ZX +3 DISK TESTER\n");

    for (i = 0; i < count; i++) {
        printf(" %s\n", items[i].label);
    }

    printf("\nUP   : F/CAPS+7\n");
    printf("DOWN : V/CAPS+6\n");
    printf("ENTER: SELECT  Q: QUIT\n");
    printf("\n\n\n\n\n\n\nSTATUS: %s", status_value);

    /* Reapply +3-style colour layout on top of terminal text output. */
    ui_attr_fill(ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 0);
    ui_attr_set_run(0, 0, 32, ZX_COLOUR_WHITE, ZX_COLOUR_BLACK, 1);
    ui_attr_set_run(13, 0, 32, ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 1);
    ui_attr_set_run(14, 0, 32, ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 1);
    ui_attr_set_run(15, 0, 32, ZX_COLOUR_BLACK, ZX_COLOUR_WHITE, 1);
    ui_attr_set_run(23, 0, 32, ZX_COLOUR_WHITE, ZX_COLOUR_BLUE, 1);
    for (col = 0; col < 8; col++) {
        ui_attr_set_cell(0, (unsigned char) (24 + col),
                         STRIPE_INK[col], STRIPE_PAPER[col], 1);
    }
    for (i = 0; i < count; i++) {
        menu_apply_row_visual(i, (unsigned char) (i == selected_index));
    }
}
