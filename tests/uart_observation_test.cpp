#include "desk_protocol/uart_observation.h"
#include <cstdlib>
#include <iostream>
#include <limits>

#define CHECK(x) do { if (!(x)) { std::cerr << __FILE__ << ':' << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)

int main() {
  desk_protocol::UARTObservation observation;
  observation.reset(123, 10);
  observation.tick(5010);
  CHECK(!observation.snapshot().seen);
  CHECK(observation.snapshot().gaps == 0);
  observation.observe_height(5010, 100, 65535);
  observation.tick(6010);
  CHECK(observation.snapshot().gaps == 0);
  observation.tick(6011);
  CHECK(observation.snapshot().gaps == 1);
  observation.tick(9000);
  CHECK(observation.snapshot().gaps == 1);
  observation.observe_height(9010, 100, 0);
  CHECK(observation.snapshot().age == 0);
  CHECK(observation.snapshot().samples == 2);
  CHECK(observation.snapshot().changes == 0);
  CHECK(observation.snapshot().sequence == 0);
  observation.observe_height(10011, 101, 1);
  CHECK(observation.snapshot().gaps == 2);
  CHECK(observation.snapshot().changes == 1);
  observation.frame_error();
  observation.transmitted();
  observation.handoff();
  const auto before = observation.snapshot();
  // Host delivery delays cannot clear cumulative UART history.
  for (uint32_t t = 10511; t < 20000; t += 500)
    observation.observe_height(t, 101, static_cast<uint16_t>(t));
  CHECK(observation.snapshot().gaps == before.gaps);
  CHECK(observation.snapshot().errors == 1 && observation.snapshot().tx == 1);
  CHECK(observation.snapshot().handoffs == 1);
  CHECK(observation.snapshot().changes == 1);

  observation.reset(456, 0xFFFFFF00u);
  observation.observe_height(0xFFFFFF00u, 20, 20);
  observation.tick(244);
  CHECK(observation.snapshot().ms == uint64_t{0xFFFFFF00u} + 500);
  CHECK(observation.snapshot().age == 500);
  observation.tick(1000);
  CHECK(observation.snapshot().gaps == 1);
  observation.tick(0xFFFFFF00u);
  observation.tick(2000);
  CHECK(observation.snapshot().age == std::numeric_limits<uint32_t>::max());
  CHECK(observation.snapshot().gaps == 1);

  observation.reset(789, 20);
  CHECK(observation.snapshot().boot == 789 && observation.snapshot().ms == 20);
  CHECK(observation.snapshot().samples == 0 && observation.snapshot().gaps == 0);
  CHECK(observation.snapshot().errors == 0 && observation.snapshot().tx == 0);
  CHECK(observation.snapshot().handoffs == 0 && !observation.snapshot().seen);
  std::cout << "UART observation diagnostics passed\n";
}
