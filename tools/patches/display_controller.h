#ifndef DESK_DISPLAY_CONTROLLER_H
#define DESK_DISPLAY_CONTROLLER_H

void display_controller_init(void);
void display_controller_render(void);
void display_controller_sleep(void);
void display_controller_restart(void);
void display_controller_native_stop(void);
void display_controller_reset_stop(int handled);
int display_controller_route(void);

#endif
