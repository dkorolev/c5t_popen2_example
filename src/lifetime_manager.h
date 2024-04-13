#pragma once

#include <iostream>
#include <atomic>
#include <mutex>
#include <thread>
#include <map>
#include <set>

#include "bricks/util/singleton.h"
#include "bricks/strings/util.h"
#include "bricks/sync/waitable_atomic.h"

// TODO(dkorolev): Refactor this.
inline void SafeStderr(std::string const& s) {
  static std::mutex m;
  std::lock_guard lock(m);
  std::cerr << s << std::endl;
}

// TODO(dkorolev): Refactor this.
inline void DumpActiveToStderr(std::string const& s) {
  SafeStderr("Active: " + s);
}

// TODO(dkorolev): Combine `TrackedInstances` with what needs to be notified of being killed!
struct LifetimeManagerSingleton final {
  struct TrackedInstances final {
    // For printing / reporting.
    // TODO(dkorolev): Unify and merge with `EnsureExactlyOnce`!
    uint64_t next_id_desc = 0u;
    std::map<uint64_t, std::string> still_alive;
  };

  struct EnsureExactlyOnce final {
    // To notify of termination exactly once.
    // TODO(dkorolev): Unify and merge with `TrackedInstances`.
    uint64_t next_id_desc = 0u;
    std::set<uint64_t> still_active;
  };

  std::atomic_bool initialized_;

  current::WaitableAtomic<std::atomic_bool> kill_switch_engaged_;
  std::atomic_bool& kill_switch_engaged_atomic_;

  current::WaitableAtomic<TrackedInstances> tracking_;
  current::WaitableAtomic<EnsureExactlyOnce> exactly_once_subscribers_;

  std::vector<std::thread> threads_to_join_;
  std::mutex threads_to_join_mutex_;

  LifetimeManagerSingleton()
      : initialized_(false),
        kill_switch_engaged_(false),
        kill_switch_engaged_atomic_(*kill_switch_engaged_.MutableScopedAccessor()) {
  }

  void MarkAsInitialized() {
    bool const was_initialized = initialized_;
    initialized_ = true;
    if (was_initialized) {
      SafeStderr("Called `MarkAsInitialized()` twice, aborting.");
      ::abort();
    }
  }

  void AbortIfNotInitialized() const {
    if (!initialized_) {
      SafeStderr("Was not `MarkAsInitialized()`, aborting.");
      ::abort();
    }
  }

  size_t TrackingAdd(std::string const& text, char const* file, size_t line) {
    AbortIfNotInitialized();
    return tracking_.MutableUse([=](TrackedInstances& trk) {
      uint64_t const id = trk.next_id_desc;
      --trk.next_id_desc;
      trk.still_alive[id] = text + " @ " + file + ':' + current::ToString(line);
      return id;
    });
  }

  void TrackingRemove(size_t id) {
    tracking_.MutableUse([=](TrackedInstances& trk) { trk.still_alive.erase(id); });
  }

  // To run "global" threads instead of `.detach()`-ing them: these threads will be `.join()`-ed upon termination.
  template <typename... ARGS>
  void EmplaceThread(ARGS&&... args) {
    AbortIfNotInitialized();
    if (!kill_switch_engaged_atomic_) {
      // It's OK to just not start the thread if already in the "terminating" mode.
      std::lock_guard lock(threads_to_join_mutex_);
      threads_to_join_.emplace_back(std::forward<ARGS>(args)...);
    }
  }

  [[nodiscard]] current::WaitableAtomicSubscriberScope SubscribeToKillSwitch(std::function<void()> f) {
    AbortIfNotInitialized();
    // Ensures that `f()` will only be called once, possibly right from the very call to `SubscribeToKillSwitch()` .
    size_t const unique_id = exactly_once_subscribers_.MutableUse([](EnsureExactlyOnce& safety) {
      uint64_t const id = safety.next_id_desc;
      safety.still_active.insert(id);
      --safety.next_id_desc;
      return id;
    });
    auto const call_f_exactly_once = [this, unique_id, f]() {
      if (exactly_once_subscribers_.MutableUse([unique_id](EnsureExactlyOnce& safety) {
            auto it = safety.still_active.find(unique_id);
            if (it == std::end(safety.still_active)) {
              return false;
            } else {
              safety.still_active.erase(it);
              return true;
            }
          })) {
        f();
      }
    };
    auto result = kill_switch_engaged_.Subscribe(call_f_exactly_once);
    if (kill_switch_engaged_atomic_) {
      call_f_exactly_once();
    }
    return result;
  }

  void DumpActive(std::function<void(std::string const&)> f = DumpActiveToStderr) const {
    AbortIfNotInitialized();
    tracking_.ImmutableUse([&f](TrackedInstances const& trk) {
      for (auto const& [_, s] : trk.still_alive) {
        f(s);
      }
    });
  }

  void WaitUntilTimeToDie() const {
    // This function is only useful when called from a thread in the scope of important data was created.
    // Generally, this is the way to create kill-switch-friendly singleton instances:
    // 1) Spawn a thread.
    // 2) Create everything in it, preferably as `Owned<WaitableAtomic<...>>`.
    // 3) At the end of this thread wait until it is time to die.
    // 4) Once it is time to die, everything this thread has created will be destroyed, gracefully or forcefully.
    AbortIfNotInitialized();
    kill_switch_engaged_.Wait([](bool die) { return die; });
  }

  void ExitForReal(int exit_code = 0, std::chrono::milliseconds graceful_delay = std::chrono::seconds(2)) {
    bool const previous_value = kill_switch_engaged_.MutableUse([](std::atomic_bool& b) {
      bool const retval = b.load();
      b = true;
      return retval;
    });

    if (previous_value) {
      SafeStderr("Ignoring a consecutive call to `ExitForReal()`.");
    } else {
      SafeStderr("`ExitForReal()` called for the first time, initating termination sequence.");
      bool ok = false;
      tracking_.WaitFor(
          [&ok](TrackedInstances const& trk) {
            if (trk.still_alive.empty()) {
              ok = true;
              return true;
            } else {
              return false;
            }
          },
          graceful_delay);
      if (ok) {
        SafeStderr("`ExitForReal()` termination sequence successful, joining the threads.");
        std::vector<std::thread> threads_to_join = [this]() {
          std::lock_guard lock(threads_to_join_mutex_);
          return std::move(threads_to_join_);
        }();
        for (auto& t : threads_to_join) {
          t.join();
        }
        SafeStderr("`ExitForReal()` termination sequence successful, all done.");
        ::exit(exit_code);
      } else {
        SafeStderr("\n`ExitForReal()` termination sequence unsuccessful, still has offenders.");
        tracking_.ImmutableUse([](TrackedInstances const& trk) {
          for (auto const& [_, s] : trk.still_alive) {
            SafeStderr("Offender: " + s);
          }
        });
        SafeStderr("\n`ExitForReal()` time to `abort()`.");
        ::abort();
      }
    }
  }
};

#define LIFETIME_MANAGER_SINGLETON() current::Singleton<LifetimeManagerSingleton>()
#define LIFETIME_MANAGER_ACTIVATE() LIFETIME_MANAGER_SINGLETON().MarkAsInitialized()

// O(1), just `.load()`-s the atomic.
#define LIFETIME_SHUTTING_DOWN LIFETIME_MANAGER_SINGLETON().kill_switch_engaged_atomic_

// Returns the `[[nodiscard]]`-ed scope for the lifetime of the passed-in lambda being registered.
#define LIFETIME_NOTIFY_OF_SHUTDOWN(f) LIFETIME_MANAGER_SINGLETON().SubscribeToKillSwitch(f)

// TODO(dkorolev): Refactor this.
#define LIFETIME_TRACKED_DEBUG_DUMP() LIFETIME_MANAGER_SINGLETON().DumpActive()

inline void LIFETIME_MANAGER_EXIT(int exit_code = 0,
                                  std::chrono::milliseconds graceful_delay = std::chrono::seconds(2)) {
  LIFETIME_MANAGER_SINGLETON().ExitForReal(exit_code, graceful_delay);
}

// This is a bit of a "singleton instance" creator.
// Not recommended to use overall, as it would create one thread per instance,
// as opposed to "a single thread to own them all". But okay for the test and for quick experiments.
// TODO(dkorolev): The same semantics could be used to leverage that "single thread to own them all".
template <class T, class... ARGS>
T& LifetimeConstructObject(std::string const& text, char const* file, int line, ARGS&&... args) {
  current::WaitableAtomic<T*> result(nullptr);
  // Construct in a dedicated thread, so that when it's time to destruct the destructors do not block one another!
  auto& s = LIFETIME_MANAGER_SINGLETON();
  s.EmplaceThread([&]() {
    size_t const id = [&]() {
      T instance(std::forward<ARGS>(args)...);
      size_t const id = s.TrackingAdd(text, file, line);
      result.SetValue(&instance);
      s.WaitUntilTimeToDie();
      return id;
    }();
    s.TrackingRemove(id);
  });
  result.Wait([](T const* ptr) { return ptr != nullptr; });
  return *result.GetValue();
}

#define KSW_IMPL2(foo, type) LifetimeConstructObject<type>(std::string() + foo + " # " + #type "()", __FILE__, __LINE__)
#define KSW_IMPL3(foo, type, arg1) \
  LifetimeConstructObject<type>(std::string() + foo + " # " + #type "(" #arg1 ")", __FILE__, __LINE__, arg1)

#define KSW_N_ARGS_IMPL3(_1, _2, _3, n, ...) n
#define KSW_N_ARGS_IMPL(args) KSW_N_ARGS_IMPL3 args

#define KSW_NARGS(...) KSW_N_ARGS_IMPL((__VA_ARGS__, 3, 2, 1, 0))

#define KSW_CHOOSER3(n) KSW_IMPL##n
#define KSW_CHOOSER2(n) KSW_CHOOSER3(n)
#define KSW_CHOOSER1(n) KSW_CHOOSER2(n)
#define KSW_CHOOSERX(n) KSW_CHOOSER1(n)

#define KWS_CONCAT_DISPATCH(x, y) x y

#define LIFETIME_TRACKED_GLOBAL(...) KWS_CONCAT_DISPATCH(KSW_CHOOSERX(KSW_NARGS(__VA_ARGS__)), (__VA_ARGS__))

// TODO(dkorolev): May make this into a variadic macro to allow passing arguments to this thread in a C+-native way.
#define LIFETIME_TRACKED_THREAD(desc, body)                    \
  LIFETIME_MANAGER_SINGLETON().EmplaceThread([&]() {           \
    auto& s = LIFETIME_MANAGER_SINGLETON();                    \
    size_t const id = s.TrackingAdd(desc, __FILE__, __LINE__); \
    body();                                                    \
    s.TrackingRemove(id);                                      \
  })

#ifdef C5T_POPEN2_H_INCLUDED

// NOTE(dkorolev): `LIFETIME_TRACKED_POPEN2` extrends the "vanilla" `popen2()` in two ways.
//
// 1) The user provides the "display name" for the inner graceful "task manager" to report what is running, and
//
// 2) The `kill()` parameter is no longer an `std::function<void()>` but `GracefulPopen2Killer`, which can be
//    both invoked with `operator()` and checked in an `if()` via its `operator bool()`.
//
// It is still up to the user to exit from the callback function that can `write()`, but
// the `LIFETIME_TRACKED_POPEN2` wrapper will ensure that once the kill switch is activated
// the original "kill-child-by-pid"` mechanism will be triggered activated without user intervention.
// The user, of course, is allowed to kill the child manually if and when needed.

inline int LIFETIME_TRACKED_POPEN2_IMPL(
    std::string const& txt,
    char const* file,
    size_t line,
    std::vector<std::string> const& cmdline,
    std::function<void(const std::string&)> cb_line,
    std::function<void(Popen2Runtime&)> cb_code = [](Popen2Runtime&) {},
    std::vector<std::string> const& env = {}) {
  return popen2(
      cmdline,
      cb_line,
      [moved_cb_code = std::move(cb_code)](Popen2Runtime& ctx) {
        // NOTE(dkorolev): On `popen2()` level it's OK to call `.Kill()` multiple times, only one will go through.
        auto const kill_switch_scope = LIFETIME_MANAGER_SINGLETON().SubscribeToKillSwitch([&ctx]() { ctx.Kill(); });
        moved_cb_code(ctx);
      },
      env);
}

#define LIFETIME_TRACKED_POPEN2(txt, ...) LIFETIME_TRACKED_POPEN2_IMPL(txt, __FILE__, __LINE__, __VA_ARGS__)

#endif  // C5T_POPEN2_H_INCLUDED