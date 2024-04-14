#include <iostream>
#include <chrono>

#include "popen2.h"
#include "lifetime_manager.h"

int main() {
  LIFETIME_MANAGER_ACTIVATE([](std::string const& s) { std::cerr << "MGR: " << s << std::endl; });
  LIFETIME_TRACKED_THREAD("run bash for 0.1 seconds", []() {
    LIFETIME_TRACKED_POPEN2(
        "echo start; sleep 0.1; echo done",
        {"bash", "-c", "echo start; sleep 0.1; echo done"},
        [](std::string const& line) { std::cerr << "bash: " << line << std::endl; });
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  LIFETIME_MANAGER_EXIT(0);
  std::cerr << "should not see this." << std::endl;
}
