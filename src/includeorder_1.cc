#include <iostream>
#include <chrono>

#include "popen2.h"
#include "lib_c5t_lifetime_manager.h"

int main() {
  auto f = [](std::string const& s) { std::cout << s << std::endl; };
  LIFETIME_TRACKED_THREAD("sleep(10s)", [&]() {
    auto const cmd = "echo hello; sleep 10; echo goodbye";
    LIFETIME_TRACKED_POPEN2(cmd, {"bash", "-c", cmd}, f);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
}
