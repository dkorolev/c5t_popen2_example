#include "bricks/sync/waitable_atomic.h"
#include "popen2.h"
#include "lifetime_manager.h"
#include "bricks/dflags/dflags.h"

DEFINE_bool(
    uncooperative,
    true,
    "Set to `false` to not start anything uncooperative, so that the code `exit(0)`-s instead of `abort()`-ing.");

// This lifetime-aware object will destruct gracefully.
struct CooperativeSlowlyDeletingObject final {
  int const value_;

  CooperativeSlowlyDeletingObject() = delete;
  explicit CooperativeSlowlyDeletingObject(int value) : value_(value) {
    // Demonstrate that `CooperativeSlowlyDeletingObject` is friendly with constructor arguments.
    SafeStderr("CooperativeSlowlyDeletingObject created.");
  }

  ~CooperativeSlowlyDeletingObject() {
    SafeStderr("Deleting the CooperativeSlowlyDeletingObject.");
    SafeStderr("CooperativeSlowlyDeletingObject deleted.");
  }
  void Dump() const {
    SafeStderr("CooperativeSlowlyDeletingObject::value_ == " + current::ToString(value_));
  }
};

// This lifetime-aware object will destruct gracefully.
struct SemiCooperativeSlowlyDeletingObject final {
  SemiCooperativeSlowlyDeletingObject() {
    SafeStderr("SemiCooperativeSlowlyDeletingObject created.");
  }

  ~SemiCooperativeSlowlyDeletingObject() {
    SafeStderr("Deleting the SemiCooperativeSlowlyDeletingObject.");
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    SafeStderr("SemiCooperativeSlowlyDeletingObject deleted.");
  }

  void Dump() const {
    SafeStderr("SemiCooperativeSlowlyDeletingObject is alive.");
  }
};

// This long-to-destruct object will force `abort()`, since the graceful shutdown delay is way under ten seconds.
struct NonCooperativeSlowlyDeletingObject final {
  NonCooperativeSlowlyDeletingObject() {
    SafeStderr("NonCooperativeSlowlyDeletingObject created.");
  }

  ~NonCooperativeSlowlyDeletingObject() {
    SafeStderr("Deleting the NonCooperativeSlowlyDeletingObject.");
    // 60 seconds is beyond the reasonable graceful shutdown wait time.
    std::this_thread::sleep_for(std::chrono::seconds(60));
    SafeStderr("[ SHOULD NOT SEE THIS ] NonCooperativeSlowlyDeletingObject deleted.");
  }

  void Dump() const {
    SafeStderr("NonCooperativeSlowlyDeletingObject is alive.");
  }
};

int main(int argc, char** argv) {
  ParseDFlags(&argc, &argv);

  LIFETIME_MANAGER_ACTIVATE();

  auto const SmallDelay = []() {
    // Just so that the terminal output comes in predictable order, since there are `bash` invocations involved.
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  };

  auto& o1 = LIFETIME_TRACKED_GLOBAL("super-cooperative instance", CooperativeSlowlyDeletingObject, 42);
  o1.Dump();
  SmallDelay();

  auto& o2 = LIFETIME_TRACKED_GLOBAL("semi-cooperative instance", SemiCooperativeSlowlyDeletingObject);
  o2.Dump();
  SmallDelay();

  if (FLAGS_uncooperative) {
    auto& o3 = LIFETIME_TRACKED_GLOBAL("[ NOT COOPERATIVE! ] offender instance", NonCooperativeSlowlyDeletingObject);
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
      SafeStderr("long super-cooperative " + current::ToString(++i));
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
    SafeStderr("long super-cooperative shutting down");
  });
  SmallDelay();

  // Will terminate after (100 .. 350) milliseconds.
  LIFETIME_TRACKED_THREAD("long operation semi-cooperative", []() {
    size_t i = 0;
    while (true) {
      if (LIFETIME_SHUTTING_DOWN) {
        SafeStderr("long semi-cooperative wait before shutting down");
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        SafeStderr("long semi-cooperative shutting down");
        break;
      } else {
        SafeStderr("long semi-cooperative " + current::ToString(++i));
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
          SafeStderr("long non-cooperative wait FOREVER=60s before shutting down");
          // 60 seconds is beyond the reasonable graceful shutdown wait time.
          std::this_thread::sleep_for(std::chrono::seconds(60));
          SafeStderr("long non-cooperative shutting down, but you will not see this =)");
          break;
        } else {
          SafeStderr("long non-cooperative " + current::ToString(++i));
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
      }
    });
    SmallDelay();
  }

  LIFETIME_TRACKED_THREAD("thread to run bash #1", []() {
    LIFETIME_TRACKED_POPEN2(
        "popen2 running bash #1",
        {"bash", "-c", "(for i in $(seq 101 109); do echo $i; sleep 1; done)"},
        [](std::string const& line) { SafeStderr("bash #1: " + line); },
        [](Popen2Runtime&) {
          // No (extra) work to do inside this `LIFETIME_TRACKED_POPEN2`, it will be gracefull shut down automatically.
        });
    // No work to do inside this `LIFETIME_TRACKED_THREAD`, as `LIFETIME_TRACKED_POPEN2` will stop itself.
  });
  SmallDelay();

  LIFETIME_TRACKED_THREAD("thread to run bash #2", []() {
    LIFETIME_TRACKED_POPEN2(
        "popen2 running bash #2",
        {"bash", "-c", "trap 'sleep 0.5; echo BYE; exit' SIGTERM; for i in $(seq 201 209); do echo $i; sleep 1; done"},
        [](std::string const& line) { SafeStderr("bash #2: " + line); });
  });
  SmallDelay();

  if (FLAGS_uncooperative) {
    // Refuses to terminate, the binary will terminate forcefully w/o waiting.
    LIFETIME_TRACKED_THREAD("[ NOT COOPERATIVE! ] thread to run bash #3", []() {
      LIFETIME_TRACKED_POPEN2(
          "[ NOT COOPERATIVE! ] popen2 running bash #3",
          {"bash", "-c", "trap 'echo REFUSING_TO_DIE' SIGTERM; for i in $(seq 301 309); do echo $i; sleep 1; done"},
          [](std::string const& line) { SafeStderr("bash #3: " + line); });
    });
  }

  SafeStderr("");
  SafeStderr("Everything started, here is what is alive as of now.");
  LIFETIME_TRACKED_DEBUG_DUMP();
  SafeStderr("Sleeping for three seconds.");
  SafeStderr("");

  std::this_thread::sleep_for(std::chrono::seconds(3));

  SafeStderr("");
  SafeStderr("Sleep done, prior to terminating here is what is alive as of now.");
  LIFETIME_TRACKED_DEBUG_DUMP();
  SafeStderr("");

  SafeStderr("Assuming the main program code is done by now, invoking `LIFETIME_MANAGER_EXIT()`");
  SafeStderr("");
  LIFETIME_MANAGER_EXIT(0);  // This will make the program terminate, one way or another, right away or after a delay.

  SafeStderr("[ SHOULD NOT SEE THIS ] Eeached the end of `main()`.");
}
