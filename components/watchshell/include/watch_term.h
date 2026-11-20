#pragma once

#include <stdbool.h>
#include <stdint.h>

void watch_term_init(void);
void watch_term_clear(void);

/* Write UTF-8/ASCII text.  `{green}...{/}` inline tags are supported. */
void watch_term_write(const char *text, uint16_t color);
void watch_term_write_tagged(const char *text, uint16_t base_color);
void watch_term_printf(uint16_t color, const char *fmt, ...);

void watch_term_set_prompt(const char *prompt);
const char *watch_term_prompt(void);

int watch_term_line_count(void);
int watch_term_scroll(void);
void watch_term_scroll_by(int delta);
void watch_term_set_scroll(int line);
void watch_term_follow(void);

/* Draw the visible terminal window.  scroll is relative to the tail. */
void watch_term_render(int x, int y, int max_lines, uint16_t default_color, bool cursor);
void watch_term_dump(void);
