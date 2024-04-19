#include <iostream>
#include <chrono>

#include "popen2.h"
#include "lib_c5t_lifetime_manager.h"

int main() {
  LIFETIME_MANAGER_SET_LOGGER([](std::string const& s) { std::cerr << "MGR: " << s << std::endl; });
  LIFETIME_TRACKED_THREAD("run bash for 0.1 seconds", []() {
    auto const cmd = "echo start; sleep 0.1; echo done";
    LIFETIME_TRACKED_POPEN2(
        cmd, {"bash", "-c", cmd}, [](std::string const& line) { std::cerr << "bash: " << line << std::endl; });
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  LIFETIME_MANAGER_EXIT(0);
  std::cerr << "should not see this." << std::endl;
}
