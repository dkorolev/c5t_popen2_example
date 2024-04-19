#include <iostream>
#include <chrono>

#include "lib_c5t_lifetime_manager.h"
#include "popen2.h"

int main() {
  LIFETIME_TRACKED_THREAD("sleep(10s)", []() {
    auto const cmd = "echo hello; sleep 10; echo goodbye";
    LIFETIME_TRACKED_POPEN2(cmd, {"bash", "-c", cmd}, [](std::string const& s) { std::cout << s << std::endl; });
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
}
