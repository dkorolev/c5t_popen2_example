#include <iostream>
#include <chrono>

#include "popen2.h"
#include "lib_c5t_lifetime_manager.h"

int main() {
  LIFETIME_MANAGER_SET_LOGGER([](std::string const& s) { std::cerr << "MGR: " << s << std::endl; });
  auto const cmd =
      "for i in $(seq 0 4); do echo $((i * 2)) >/dev/stdout; echo $((i * 2 + 1)) >/dev/stderr; sleep 0.1; done";
  LIFETIME_TRACKED_POPEN2(
      cmd, {"bash", "-c", cmd}, [](std::string const& line) { std::cerr << "bash: " << line << std::endl; });
}
