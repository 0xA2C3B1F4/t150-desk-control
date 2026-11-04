#ifndef DESK_DISPLAY_CORE_H
#define DESK_DISPLAY_CORE_H

#include <stdbool.h>
#include <stdint.h>

enum { DISPLAY_AUTO, DISPLAY_NORMAL };
enum {
  DISPLAY_STATE_AUTO = 1,
  DISPLAY_STATE_ATTENTION = 2,
  DISPLAY_STATE_TIME_LEFT = 4,
  DISPLAY_STATE_BLANK = 8,
  DISPLAY_STATE_UNAVAILABLE = 16
};

typedef struct {
  uint32_t magic;
  uint32_t deadline;
  uint16_t timeout_seconds;
  uint8_t mode;
  uint8_t attention;
  uint8_t blank;
  uint8_t wake_pending;
  uint8_t expired;
  uint8_t reserved;
} DisplayState;

void display_init(DisplayState* state, uint32_t now);
bool display_valid(const DisplayState* state);
bool display_configure(DisplayState* state, uint8_t mode, uint16_t seconds, uint32_t now);
void display_restart(DisplayState* state, uint32_t now);
void display_sleep(DisplayState* state);
bool display_render_blank(DisplayState* state, uint32_t now, bool attention);
uint8_t display_status(const DisplayState* state, uint32_t now, bool attention);

#endif
