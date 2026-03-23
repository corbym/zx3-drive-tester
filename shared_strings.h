#pragma once

/*
 * Shared immutable strings used across translation units.
 *
 * Paging-ready hook: redefine ZX3_STR_STORAGE at build time to place these
 * constants into a dedicated section/bank when a paging strategy is enabled.
 * Default keeps current placement and behavior.
 */
#ifndef ZX3_STR_STORAGE
#define ZX3_STR_STORAGE
#endif

extern const char zx3_str_dash3[];
extern const char zx3_str_fail[];
extern const char zx3_str_stopped[];
extern const char zx3_str_invalid[];
extern const char zx3_str_timeout[];
extern const char zx3_str_not_ready[];
extern const char zx3_str_error[];
extern const char zx3_str_check_media[];
extern const char zx3_str_out_of_range[];
extern const char zx3_str_skipped[];

extern const char zx3_label_result[];
extern const char zx3_ctrl_enter_esc_menu[];
extern const char zx3_ctrl_auto_return_menu[];

