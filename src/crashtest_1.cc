#include <iostream>

#include "popen2.h"
#include "lib_c5t_lifetime_manager.h"

int main() {
  LIFETIME_MANAGER_SET_LOGGER([](std::string const& s) { std::cerr << "MGR: " << s << std::endl; });
  LIFETIME_TRACKED_POPEN2("echo start; sleep 0.1; echo done",
                          {"bash", "-c", "echo start; sleep 0.1; echo done"},
                          [](std::string const& line) { std::cerr << "bash: " << line << std::endl; });
  LIFETIME_MANAGER_EXIT(0);
  std::cerr << "should not see this." << std::endl;
}
