#include <iostream>
#include <chrono>

#include "popen2.h"
#include "lifetime_manager.h"

int main() {
  LIFETIME_MANAGER_ACTIVATE([](std::string const& s) { std::cerr << "MGR: " << s << std::endl; });
  LIFETIME_TRACKED_THREAD("run bash for 0.1 seconds", []() {
    auto const cmd = "for i in $(seq 50); do echo $i; sleep 0.1; done";
    LIFETIME_TRACKED_POPEN2(
        cmd, {"bash", "-c", cmd}, [](std::string const& line) { std::cerr << "bash: " << line << std::endl; });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  std::cerr << "natural, organic exit." << std::endl;  // Delibrately no `LIFETIME_MANAGER_EXIT(0)`.
}
