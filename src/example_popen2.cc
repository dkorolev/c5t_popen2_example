#include <chrono>
#include <iostream>
#include <thread>
#include <csignal>

#include "bricks/util/singleton.h"
#include "popen2.h"  // IWYU pragma: keep

#include "bricks/dflags/dflags.h"
#include "bricks/time/chrono.h"

DEFINE_double(wait_s, 0.0, "Set to wait instead of running the test.");
DEFINE_bool(killable, true, "Set to `false` to disable killing the waiting program gracefully.");

void handler(int n) {
  if (FLAGS_killable) {
    std::cout << "obeying signal " << n << std::endl;
    std::exit(0);
  } else {
    std::cout << "ignoring signal " << n << std::endl;
  }
}

struct ScopedObject final {
  ScopedObject() {
    std::cout << "constructor" << std::endl;
  }
  ~ScopedObject() {
    std::cout << "destructor" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    std::cout << "took 0.5 seconds to destruct" << std::endl;
  }
};

int main(int argc, char* argv[]) {
  ParseDFlags(&argc, &argv);
  if (FLAGS_wait_s) {
    signal(SIGTERM, handler);

    // NOTE(dkorolev): A global object or a singleton will be destructed. A local object will *NOT*!
    current::Singleton<ScopedObject>();
    // ScopedObject this_will_not_be_destructed_if_killed;

    std::cout << "sleeping " << FLAGS_wait_s << " seconds" << (FLAGS_killable ? ", killable" : "") << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(FLAGS_wait_s * 1e3)));
    std::cout << "sleep done" << std::endl;
  } else {
    auto const bin = current::Singleton<dflags::Argv0Container>().argv_0;

    auto const run = [](std::vector<std::string> const& args, double kill_delay_s) {
      auto const t0 = current::time::Now();
      bool destructed = false;
      std::cout << "$";
      for (auto const& e : args) {
        std::cout << ' ' << e;
      }
      std::cout << std::endl;
      popen2(
          args,
          [&destructed](std::string const& line) {
            std::cout << "  " << line << std::endl;
            if (line == "destructor") {
              if (destructed) {
                std::cout << "! ERROR: destructor called twice" << std::endl;
              } else {
                destructed = true;
              }
            }
          },
          [kill_delay_s](std::function<void(std::string const&)> write, std::function<void()> kill) {
            static_cast<void>(write);
            std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(kill_delay_s * 1e3)));
            std::cout << std::fixed << std::setprecision(1) << "# killing after " << kill_delay_s << " seconds"
                      << std::endl;
            kill();
          });
      auto const t1 = current::time::Now();
      std::cout << std::fixed << std::setprecision(1) << "# ran for " << (t1 - t0).count() * 1e-6 << " seconds"
                << std::endl;
      if (!destructed) {
        std::cout << "! ERROR: destructor not called" << std::endl;
      }
      std::cout << std::endl;
    };

    run({bin, "--wait_s=2.0"}, 1.0);
    run({bin, "--wait_s=2.0", "--killable=false"}, 1.0);

    std::cout << "# end of demo" << std::endl;
  }
  return 0;
}
