#ifndef DESK_LOCAL_LOCK_CONTROLLER_H
#define DESK_LOCAL_LOCK_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

void local_lock_controller_init(void);
void local_lock_controller_poll(void);
void local_lock_controller_before_sleep(void);
void local_lock_controller_after_wake(void);
bool local_lock_controller_inputs_blocked(void);
bool local_lock_controller_motion_blocked(void);
bool local_lock_controller_sleep_blocked(void);
int local_lock_controller_route(void);
bool local_lock_controller_render(void);
bool local_lock_admitted(void);
uint32_t local_lock_request(uint32_t desired);
void local_lock_native_stop(void);

#endif
