/* Build the maintained TB01 implementation against the shared RAM ABI. */
#include "platform_runtime.h"
#define TB01_STATE_ADDRESS PLATFORM_BUZZER_ADDRESS
#define TB01_READY() platform_ready()
#define TB01_INITIALIZED() platform_initialized()
#define TB01_MARK_READY() platform_modules_ready()
#define TB01_ACCEPT_VERSION(version) ((version) == 1)
#define TB01_EXTENSION_ONLY
__attribute__((noinline)) int tb01_extension_route(void);
#define tb01_route tb01_extension_route
#include "tb01.c"
#undef tb01_route
/* Keep extension request buffers out of native command call stacks. */
int tb01_route(void) {
  int original = FN(0x080097e1u, int (*)(void))();
  return original ? original : tb01_extension_route();
}
