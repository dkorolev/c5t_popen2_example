#include <iostream>
#include <chrono>

#include "popen2.h"
#include "lifetime_manager.h"

int main() {
  LIFETIME_MANAGER_ACTIVATE([](std::string const& s) { std::cerr << "MGR: " << s << std::endl; });
  LIFETIME_TRACKED_THREAD("sleep(0.1s)", []() {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  LIFETIME_MANAGER_EXIT(0);
  std::cerr << "should not see this." << std::endl;
}
