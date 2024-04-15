#include <mutex>

#include "bricks/sync/waitable_atomic.h"
#include "popen2.h"
#include "lib_c5t_lifetime_manager.h"
#include "bricks/dflags/dflags.h"
#include "bricks/strings/printf.h"

DEFINE_bool(
    uncooperative,
    true,
    "Set to `false` to not start anything uncooperative, so that the code `exit(0)`-s instead of `abort()`-ing.");

inline static std::mutex output_mutex;
inline void ThreadSafeLog(std::string const& s) {
  std::lock_guard lock(output_mutex);
  std::cout << s << std::endl;
}

// This lifetime-aware object will destruct gracefully.
struct CooperativeSlowlyDeletingObject final {
  int const value_;

  CooperativeSlowlyDeletingObject() = delete;
  explicit CooperativeSlowlyDeletingObject(int value) : value_(value) {
    // Demonstrate that `CooperativeSlowlyDeletingObject` is friendly with constructor arguments.
    ThreadSafeLog("CooperativeSlowlyDeletingObject created.");
  }

  ~CooperativeSlowlyDeletingObject() {
    ThreadSafeLog("Deleting the CooperativeSlowlyDeletingObject.");
    ThreadSafeLog("CooperativeSlowlyDeletingObject deleted.");
  }
  void Dump() const {
    ThreadSafeLog("CooperativeSlowlyDeletingObject::value_ == " + current::ToString(value_));
  }
};

// This lifetime-aware object will destruct gracefully.
struct SemiCooperativeSlowlyDeletingObject final {
  SemiCooperativeSlowlyDeletingObject() {
    ThreadSafeLog("SemiCooperativeSlowlyDeletingObject created.");
  }

  ~SemiCooperativeSlowlyDeletingObject() {
    ThreadSafeLog("Deleting the SemiCooperativeSlowlyDeletingObject.");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    ThreadSafeLog("SemiCooperativeSlowlyDeletingObject deleted.");
  }

  void Dump() const {
    ThreadSafeLog("SemiCooperativeSlowlyDeletingObject is alive.");
  }
};

// This long-to-destruct object will force `abort()`, since the graceful shutdown delay is way under ten seconds.
struct NonCooperativeSlowlyDeletingObject final {
  NonCooperativeSlowlyDeletingObject() {
    ThreadSafeLog("NonCooperativeSlowlyDeletingObject created.");
  }

  ~NonCooperativeSlowlyDeletingObject() {
    ThreadSafeLog("Deleting the NonCooperativeSlowlyDeletingObject.");
    // 60 seconds is beyond the reasonable graceful shutdown wait time.
    std::this_thread::sleep_for(std::chrono::seconds(60));
    ThreadSafeLog("[ SHOULD NOT SEE THIS ] NonCooperativeSlowlyDeletingObject deleted.");
  }

  void Dump() const {
    ThreadSafeLog("NonCooperativeSlowlyDeletingObject is alive.");
  }
};

int main(int argc, char** argv) {
  ParseDFlags(&argc, &argv);

  // The lifetime manager ensures the log functions are called in the thread-safe way.
  LIFETIME_MANAGER_ACTIVATE([](std::string const& s) { std::cerr << "MGR: " << s << std::endl; });

  auto const SmallDelay = []() {
    // Just so that the terminal output comes in predictable order, since there are `bash` invocations involved.
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  };

  auto& o1 = LIFETIME_TRACKED_INSTANCE(CooperativeSlowlyDeletingObject, "super-cooperative instance", 42);
  o1.Dump();
  SmallDelay();

  auto& o2 = LIFETIME_TRACKED_INSTANCE(SemiCooperativeSlowlyDeletingObject, "semi-cooperative instance");
  o2.Dump();
  SmallDelay();

  if (FLAGS_uncooperative) {
    auto& o3 = LIFETIME_TRACKED_INSTANCE(NonCooperativeSlowlyDeletingObject, "[ NOT COOPERATIVE! ] offender instance");
    o3.Dump();
    SmallDelay();
  }

  // Will terminate right away.
  LIFETIME_TRACKED_THREAD("long operation super-cooperative", []() {
    current::WaitableAtomic<bool> done(false);
    auto const scope = LIFETIME_NOTIFY_OF_SHUTDOWN([&done]() { done.SetValue(true); });
    size_t i = 0;
    bool truly_done = false;
    while (!truly_done) {
      ThreadSafeLog("long super-cooperative " + current::ToString(++i));
      done.WaitFor(
          [&truly_done](bool b) {
            if (b) {
              truly_done = true;
              return true;
            } else {
              return false;
            }
          },
          std::chrono::milliseconds(250));
    }
    ThreadSafeLog("long super-cooperative shutting down");
  });
  SmallDelay();

  // Will terminate after (100 .. 350) milliseconds.
  LIFETIME_TRACKED_THREAD("long operation semi-cooperative", []() {
    size_t i = 0;
    while (true) {
      if (LIFETIME_SHUTTING_DOWN) {
        ThreadSafeLog("long semi-cooperative wait before shutting down");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        ThreadSafeLog("long semi-cooperative shutting down");
        break;
      } else {
        ThreadSafeLog("long semi-cooperative " + current::ToString(++i));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  });
  SmallDelay();

  if (FLAGS_uncooperative) {
    // Takes a whole minute to terminate, the binary will terminate forcefully w/o waiting.
    LIFETIME_TRACKED_THREAD("[ NOT COOPERATIVE! ] long operation non-cooperative", []() {
      size_t i = 0;
      while (true) {
        if (LIFETIME_SHUTTING_DOWN) {
          ThreadSafeLog("long non-cooperative wait FOREVER=60s before shutting down");
          // 60 seconds is beyond the reasonable graceful shutdown wait time.
          std::this_thread::sleep_for(std::chrono::seconds(60));
          ThreadSafeLog("long non-cooperative shutting down, but you will not see this =)");
          break;
        } else {
          ThreadSafeLog("long non-cooperative " + current::ToString(++i));
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    });
    SmallDelay();
  }

  LIFETIME_TRACKED_THREAD("thread to run bash #1", []() {
    LIFETIME_TRACKED_POPEN2(
        "popen2 running bash #1",
        {"bash", "-c", "(for i in $(seq 101 199); do echo $i; sleep 0.25; done)"},
        [](std::string const& line) { ThreadSafeLog("bash #1: " + line); },
        [](Popen2Runtime&) {
          // No (extra) work to do inside this `LIFETIME_TRACKED_POPEN2`, it will be gracefull shut down automatically.
        });
    // No work to do inside this `LIFETIME_TRACKED_THREAD`, as `LIFETIME_TRACKED_POPEN2` will stop itself.
  });
  SmallDelay();

  LIFETIME_TRACKED_THREAD("thread to run bash #2", []() {
    LIFETIME_TRACKED_POPEN2(
        "popen2 running bash #2",
        {"bash", "-c", "trap 'sleep 1; echo BYE; exit' SIGTERM; for i in $(seq 201 299); do echo $i; sleep 0.25; done"},
        [](std::string const& line) { ThreadSafeLog("bash #2: " + line); },
        [](Popen2Runtime&) { LIFETIME_SLEEP_UNTIL_SHUTDOWN(); });
  });
  SmallDelay();

  if (FLAGS_uncooperative) {
    // Refuses to terminate, the binary will terminate forcefully w/o waiting.
    LIFETIME_TRACKED_THREAD("[ NOT COOPERATIVE! ] thread to run bash #3", []() {
      LIFETIME_TRACKED_POPEN2(
          "[ NOT COOPERATIVE! ] popen2 running bash #3",
          {"bash", "-c", "trap 'echo NOT_DYING' SIGTERM; for i in $(seq 301 399); do echo $i; sleep 0.25; done"},
          [](std::string const& line) { ThreadSafeLog("bash #3: " + line); },
          [](Popen2Runtime&) { LIFETIME_SLEEP_UNTIL_SHUTDOWN(); });
    });
  }
  SmallDelay();

  // Also test that all is well if a POPEN2 process has terminated before `LIFETIME_MANAGER_EXIT()` is invoked.
  LIFETIME_TRACKED_THREAD("thread to run bash #4", []() {
    LIFETIME_TRACKED_POPEN2("popen2 running bash #4",
                            {"bash", "-c", "echo dead in 0.5 seconds; sleep 0.5; echo dead"},
                            [](std::string const& line) { ThreadSafeLog("bash #4: " + line); });
  });
  SmallDelay();

  auto const DumpLifetimeTrackedInstance = [](LifetimeTrackedInstance const& t) {
    ThreadSafeLog(current::strings::Printf("- %s @ %s:%d, up %.3lfs",
                                           t.description.c_str(),
                                           t.file_basename.c_str(),
                                           t.line_as_number,
                                           1e-6 * (current::time::Now() - t.t_added).count()));
  };

  ThreadSafeLog("");
  ThreadSafeLog("Everything started, here is what is alive as of now.");
  LIFETIME_TRACKED_DEBUG_DUMP(DumpLifetimeTrackedInstance);
  ThreadSafeLog("Sleeping for three seconds.");
  ThreadSafeLog("");

  std::this_thread::sleep_for(std::chrono::seconds(3));

  ThreadSafeLog("");
  ThreadSafeLog("Sleep done, prior to terminating here is what is alive as of now.");
  LIFETIME_TRACKED_DEBUG_DUMP(DumpLifetimeTrackedInstance);
  ThreadSafeLog("");

  ThreadSafeLog("Assuming the main program code is done by now, invoking `LIFETIME_MANAGER_EXIT().`");
  ThreadSafeLog("");
  LIFETIME_MANAGER_EXIT(0);  // This will make the program terminate, one way or another, right away or after a delay.

  ThreadSafeLog("[ SHOULD NOT SEE THIS ] Eeached the end of `main()`.");
}
