#ifndef DESK_PLATFORM_RUNTIME_H
#define DESK_PLATFORM_RUNTIME_H
#include <stdbool.h>
#include <stdint.h>
#include "platform_layout.h"
#define PLATFORM_VALID_MAGIC 0x46503031u
#define PLATFORM_GUARD_WORD 0x51a7c38du
bool platform_profile_matches(void);
bool platform_initialized(void);
bool platform_ready(void);
void platform_modules_ready(void);
void platform_bootstrap(void);
#endif
